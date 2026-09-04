// StaticCostModel ("static-v1") — the Phase 1 baseline (ARCHITECTURE §6.2, §8).
//
// Nothing here is learned. Every rate comes from the shared seed table
// (router/cost/seeds.h), which derives decode from device bandwidth over model
// size rather than looking a model up by name — a table indexed by name would
// be useless on the first unseen model, whereas this degrades to "roughly
// right" for anything.
//
// Phase 2's LearnedCostModel replaces all of it via observe(). Keeping this one
// inert is what makes the Phase 2 improvement measurable rather than assumed.

#include <algorithm>
#include <cmath>

#include "common/config.h"
#include "common/util.h"
#include "router/core/interfaces.h"
#include "router/cost/seeds.h"

namespace rf {
namespace {

class StaticCostModel : public ICostModel {
public:
    explicit StaticCostModel(const ScoringConfig& scoring) : scoring_(scoring) {}

    const char* name() const override { return "static-v1"; }

    uint64_t footprint_bytes(const std::string& model, const NodeState& node,
                             uint32_t num_ctx) const override {
        return seed_footprint_bytes(model, node, num_ctx, scoring_);
    }

    OutputPrediction predict_output(const RequestFeatures& req) const override {
        OutputPrediction p;
        const bool capped = req.predicted_output_tokens > 0;
        p.tokens = capped ? req.predicted_output_tokens : seed_output_tokens();
        p.sigma = capped ? req.predicted_output_sigma : seed_output_sigma();
        p.conf = Confidence::Seeded;
        return p;
    }

    Estimate estimate(const RequestFeatures& req, const NodeState& node,
                      const LedgerView& ledger, const EvictionPlan& evict,
                      int64_t now_ms) const override {
        Estimate e;
        e.conf = Confidence::Seeded;
        e.samples = 0;

        const uint64_t footprint = footprint_bytes(req.model, node, req.num_ctx);
        const SeedRates rates = seed_rates(node, footprint, scoring_);

        // --- decode, with contention priced exactly once (§6.2, D5) ---------
        const uint32_t decoders = ledger.concurrent_decoders(node.id);
        const double concurrent = static_cast<double>(decoders) + 1.0;  // this one
        const double effective_decode =
            rates.decode_tokens_per_ms /
            (1.0 + scoring_.contention_alpha * (concurrent - 1.0));

        const OutputPrediction out = predict_output(req);
        e.t_decode_ms =
            static_cast<double>(out.tokens) / std::max(1e-9, effective_decode);

        // --- prefill --------------------------------------------------------
        e.t_prefill_ms = static_cast<double>(req.prompt_tokens) /
                         std::max(1e-9, rates.prefill_tokens_per_ms);

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
            e.t_load_ms = static_cast<double>(footprint) / rates.load_bytes_per_ms;
        }

        // --- queue: true queueing only (§6.2, D6) ---------------------------
        const uint32_t slots = std::max(1u, node.engine_slots);
        const uint32_t inflight = ledger.inflight(node.id);
        if (inflight < slots) {
            e.t_queue_ms = 0;
        } else {
            // A whole job's worth of work per queued round, excluding load time
            // by construction — averaging cold starts into the queue term is
            // what made rev 1 read busy nodes as busier than they are.
            const double warm_job_ms = e.t_prefill_ms + e.t_decode_ms;
            const uint32_t ahead = inflight - slots + 1;
            const double rounds =
                std::ceil(static_cast<double>(ahead) / static_cast<double>(slots));
            e.t_queue_ms = rounds * warm_job_ms;
        }

        // --- eviction externality (§6.2, D7) --------------------------------
        // What this request will cost the *next* one by taking the VRAM. Only
        // charged when the victim was used recently: dropping a model nobody
        // has asked for in an hour costs nothing worth pricing.
        if (evict.bytes > 0 && evict.evicts_warm && scoring_.evict_weight > 0) {
            e.t_evict_ms = static_cast<double>(evict.bytes) /
                           rates.load_bytes_per_ms * scoring_.evict_weight;
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
    s.ewma_halflife = cfg.get_num("cost.ewma_halflife", s.ewma_halflife);

    const Json& seeds = cfg.get("cost.gpu_seeds");
    for (size_t i = 0; i < seeds.size(); ++i) {
        const Json& e = seeds.at(i);
        GpuSeed g;
        g.match = e["match"].as_str();
        g.mem_bw = e["mem_bw_bytes_per_ms"].as_num(0);
        g.load_bw = e["load_bw_bytes_per_ms"].as_num(0);
        g.prefill_ratio = e["prefill_ratio"].as_num(40);
        if (g.match.empty() || g.mem_bw <= 0 || g.load_bw <= 0) {
            RF_WARN("cost.gpu_seeds[%zu] ignored: needs a non-empty match and "
                    "positive bandwidths", i);
            continue;
        }
        s.gpu_seeds.push_back(std::move(g));
    }
    return s;
}

}  // namespace rf
