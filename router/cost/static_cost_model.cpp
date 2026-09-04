// StaticCostModel ("static-v1") — the Phase 1 seed (ARCHITECTURE §6.2, §8).
//
// Nothing here is learned. Every rate comes from a small table keyed by GPU
// class, or from first principles where that is better than a table:
//
//   decode is memory-bound     -> tokens/ms ~ memory_bandwidth / weights_bytes
//   prefill is compute-bound   -> a per-class multiple of the decode rate
//   load is transfer-bound     -> a per-class bytes/ms
//
// The bandwidth-derived decode rate matters: a lookup table indexed by model
// name would be useless on the first unseen model, whereas this degrades to
// "roughly right" for anything. Phase 2 replaces all of it via observe(), and
// the point of keeping this model honest is that the Phase 2 improvement is
// then measurable rather than assumed.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/config.h"
#include "common/util.h"
#include "router/core/interfaces.h"

namespace rf {
namespace {

constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

// Seeds per GPU class. `mem_bw` is device memory bandwidth in bytes/ms;
// `load_bw` is the rate at which weights reach VRAM from storage, which is a
// different and much slower path.
struct GpuSeed {
    const char* match;       // case-insensitive substring of the device name
    double mem_bw;           // bytes per ms
    double load_bw;          // bytes per ms
    double prefill_ratio;    // prefill tokens/ms as a multiple of decode
};

// Measured on this project's development machine where marked; the rest are
// vendor figures. All of them are starting points, not claims.
const GpuSeed kGpuSeeds[] = {
    // Jetson: unified LPDDR5, and loading comes off the same memory, so the
    // load path is unusually fast relative to the compute.
    {"orin",     102e6,  1.2e6, 30},
    {"xavier",    59e6,  0.8e6, 30},
    {"jetson",    68e6,  1.0e6, 30},
    // Ada laptop parts. 4060 Laptop: 272 GB/s; load_bw 0.5e6 bytes/ms was
    // measured here (4.75 GB resident in ~9.5 s of load).
    {"rtx 40",   272e6,  0.5e6, 45},
    {"rtx 30",   448e6,  0.5e6, 45},
    {"rtx 20",   448e6,  0.4e6, 40},
    {"rtx 50",   896e6,  0.7e6, 50},
    {"a100",   1555e6,  2.0e6, 60},
    {"h100",   3350e6,  3.0e6, 60},
};

const GpuSeed kDefaultSeed = {"", 200e6, 0.5e6, 40};

const GpuSeed& seed_for(const std::string& gpu_name) {
    const std::string name = lower(gpu_name);
    for (const auto& s : kGpuSeeds)
        if (name.find(s.match) != std::string::npos) return s;
    return kDefaultSeed;
}

// KV cache per token, seeded from weight size. A 7B q4 model with grouped-query
// attention costs roughly 56-76 KB/token; scaling linearly with weights lands
// in the right neighbourhood for the 3B-70B range this project targets. Phase 2
// corrects it from the observed VRAM delta (D2).
constexpr double kKvBytesPerTokenPerGiB = 16.0 * 1024.0;

// Used when the engine reports no size for a model at all. Deliberately large:
// under-estimating a footprint admits a node that cannot serve the request,
// which is a routing failure, while over-estimating only loses a candidate.
constexpr uint64_t kUnknownModelBytes = 6ULL * 1024 * 1024 * 1024;

// Seeded output-length prior. The sigma is intentionally near the mean: we know
// almost nothing before the request, and §6.2's noise check depends on that
// being stated honestly rather than flattered.
constexpr uint32_t kSeedOutputTokens = 256;
constexpr float kSeedOutputSigma = 200.f;

class StaticCostModel : public ICostModel {
public:
    explicit StaticCostModel(const ScoringConfig& scoring) : scoring_(scoring) {}

    const char* name() const override { return "static-v1"; }

    uint64_t footprint_bytes(const std::string& model, const NodeState& node,
                             uint32_t num_ctx) const override {
        // Best evidence first: if the engine already holds it, that is the
        // measured answer and beats any estimate.
        const uint64_t resident = node.resident_bytes(model);
        if (resident > 0) return resident;

        uint64_t weights = node.disk_bytes(model);
        double margin = scoring_.footprint_margin;
        if (weights == 0) {
            weights = kUnknownModelBytes;
            margin = 1.0;  // already a pessimistic guess; do not inflate twice
        }

        const double weights_gib = static_cast<double>(weights) / kGiB;
        const double ctx = num_ctx > 0 ? num_ctx : 4096;
        const double kv = ctx * kKvBytesPerTokenPerGiB * weights_gib;
        return static_cast<uint64_t>(static_cast<double>(weights) * margin + kv);
    }

    OutputPrediction predict_output(const RequestFeatures& req) const override {
        OutputPrediction p;
        p.tokens = req.predicted_output_tokens > 0 ? req.predicted_output_tokens
                                                   : kSeedOutputTokens;
        p.sigma = req.predicted_output_tokens > 0 ? req.predicted_output_sigma
                                                  : kSeedOutputSigma;
        p.conf = Confidence::Seeded;
        return p;
    }

    Estimate estimate(const RequestFeatures& req, const NodeState& node,
                      const LedgerView& ledger, const EvictionPlan& evict,
                      int64_t now_ms) const override {
        Estimate e;
        e.conf = Confidence::Seeded;
        e.samples = 0;

        const GpuSeed& seed = seed_for(node.gpu_name);
        const uint64_t footprint = footprint_bytes(req.model, node, req.num_ctx);
        const double weights = std::max(1.0, static_cast<double>(footprint));

        // --- decode, with contention priced exactly once (§6.2, D5) ---------
        const double base_decode = seed.mem_bw / weights;  // tokens per ms
        const uint32_t decoders = ledger.concurrent_decoders(node.id);
        const double concurrent = static_cast<double>(decoders) + 1.0;  // this request
        const double effective_decode =
            base_decode / (1.0 + scoring_.contention_alpha * (concurrent - 1.0));

        const OutputPrediction out = predict_output(req);
        e.t_decode_ms = static_cast<double>(out.tokens) / std::max(1e-9, effective_decode);

        // --- prefill --------------------------------------------------------
        const double prefill_rate = base_decode * seed.prefill_ratio;
        e.t_prefill_ms = static_cast<double>(req.prompt_tokens) /
                         std::max(1e-9, prefill_rate);

        // --- load (§6.2: the term that justifies the project) ---------------
        if (!node.residency_known) {
            // The engine cannot tell us what is warm. Assuming "cold" would
            // invent the one number this project exists to measure, so the term
            // is omitted and flagged rather than guessed (§10, D3).
            e.t_load_ms = 0;
            e.omitted_terms |= kTermLoad;
        } else if (node.is_resident(req.model)) {
            e.t_load_ms = 0;
        } else if (const PendingLoad* p = ledger.pending(node.id, req.model)) {
            // Already loading here because of an earlier request: wait for the
            // remainder rather than starting a duplicate load elsewhere (D4).
            const double elapsed = static_cast<double>(now_ms - p->started_ms);
            e.t_load_ms = std::max(0.0, p->eta_ms - elapsed);
        } else {
            e.t_load_ms = static_cast<double>(footprint) / seed.load_bw;
        }

        // --- queue: true queueing only (§6.2, D6) ---------------------------
        const uint32_t slots = std::max(1u, node.engine_slots);
        const uint32_t inflight = ledger.inflight(node.id);
        if (inflight < slots) {
            e.t_queue_ms = 0;
        } else {
            // A whole job's worth of work per queued round. This model's own
            // prediction stands in for mean_warm_job_ms, and it excludes load
            // time by construction — averaging cold starts into the queue term
            // is what made rev 1 read busy nodes as busier than they are.
            const double warm_job_ms = e.t_prefill_ms + e.t_decode_ms;
            const uint32_t ahead = inflight - slots + 1;
            const double rounds = std::ceil(static_cast<double>(ahead) /
                                            static_cast<double>(slots));
            e.t_queue_ms = rounds * warm_job_ms;
        }

        // --- eviction externality (§6.2, D7) --------------------------------
        // What this request will cost the *next* one by taking the VRAM. Only
        // charged when the victim was used recently: dropping a model nobody
        // has asked for in an hour costs nothing worth pricing.
        if (evict.bytes > 0 && evict.evicts_warm && scoring_.evict_weight > 0) {
            e.t_evict_ms = static_cast<double>(evict.bytes) / seed.load_bw *
                           scoring_.evict_weight;
        }

        // --- thermal, opt-in and only with real telemetry -------------------
        if (scoring_.thermal_penalty) {
            if (node.telemetry_ok && node.power_cap_watts > 0 && node.power_watts > 0) {
                const double headroom =
                    1.0 - std::min(1.0, static_cast<double>(node.power_watts) /
                                            static_cast<double>(node.power_cap_watts));
                e.t_decode_ms += scoring_.thermal_weight_ms * (1.0 - headroom);
            } else {
                e.omitted_terms |= kTermThermal;
            }
        }

        // --- uncertainty (§6.2, D8) -----------------------------------------
        // Dominated by output length; a cold load contributes its own residual
        // because a seeded load bandwidth is itself a guess.
        const double sigma_decode = out.sigma / std::max(1e-9, effective_decode);
        const double sigma_load = e.t_load_ms * 0.30;
        e.sigma_ms = std::sqrt(sigma_decode * sigma_decode + sigma_load * sigma_load);
        return e;
    }

    void observe(const TraceRecord&) override {
        // Static by definition. Phase 2's LearnedCostModel is what consumes
        // these; keeping this one inert is what makes the comparison mean
        // something.
    }

private:
    ScoringConfig scoring_;
};

}  // namespace

std::unique_ptr<ICostModel> make_static_cost_model(const ScoringConfig& scoring) {
    return std::unique_ptr<ICostModel>(new StaticCostModel(scoring));
}

ScoringConfig ScoringConfig::from_config(const Config& cfg) {
    ScoringConfig s;
    s.evict_weight = cfg.get_num("scoring.evict_weight", s.evict_weight);
    s.warm_window_ms = cfg.get_int("scoring.warm_window_ms", s.warm_window_ms);
    s.thermal_penalty = cfg.get_bool("scoring.thermal_penalty", s.thermal_penalty);
    s.thermal_weight_ms = cfg.get_num("scoring.thermal_weight_ms", s.thermal_weight_ms);
    s.node_stale_ms = cfg.get_int("scoring.node_stale_ms", s.node_stale_ms);
    s.footprint_margin = cfg.get_num("scoring.footprint_margin", s.footprint_margin);
    s.contention_alpha = cfg.get_num("scoring.contention_alpha", s.contention_alpha);
    return s;
}

}  // namespace rf
