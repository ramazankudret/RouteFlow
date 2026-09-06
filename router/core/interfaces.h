// RouteFlow — router-side interfaces (ARCHITECTURE §5).
//
// Each build phase swaps one implementation behind these without touching the
// others: Phase 1 replaces the policy, Phase 2 the cost model, Phase 3 adds the
// placement manager. Nothing in the router depends on a concrete type.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/types.h"

namespace rf {


// One row of the cost model's hardware seed table (§8: "a static table keyed by
// GPU class so a fresh cluster is not useless on its first request").
//
// This is configuration, not a constant, because the built-in table is coarse
// by construction: memory bandwidth varies more *within* a GPU generation than
// between generations — a 4060 Laptop is 272 GB/s and a 4090 is 1008 — so
// keying on a generation substring is a starting point, not an answer. An
// operator who knows their hardware states it; Phase 2 learns the correction
// either way.
struct GpuSeed {
    std::string match;      // case-insensitive substring of the device name
    double mem_bw = 0;      // device memory bandwidth, bytes/ms (drives decode)
    double load_bw = 0;     // storage -> VRAM, bytes/ms (drives T_load)
    double prefill_ratio = 40;  // prefill tokens/ms as a multiple of decode
};

// Tunables that change how a score is computed. Kept in one struct so that a
// policy comparison run can state its scoring configuration in one line, and so
// that no term is controlled by a constant buried in a .cpp.
struct ScoringConfig {
    // §6.2. Priced by default: without it the policy is self-defeating under
    // memory pressure. Set to 0 to reproduce the rev-1 behaviour.
    double evict_weight = 0.5;
    // A model used within this window is "warm"; evicting it costs the next
    // request a reload, which is what evict_weight prices.
    int64_t warm_window_ms = 120000;

    // Off by default (§6.2). Requires telemetry; otherwise the term is omitted
    // and flagged rather than treated as zero.
    bool thermal_penalty = false;
    double thermal_weight_ms = 0;

    // A node whose last sample is older than this is not a candidate (§6.1).
    int64_t node_stale_ms = 5000;

    // Applied to a footprint estimate only while it has no observations (D2).
    // 1.08, not the 1.15 rev 2 first specified: measured overhead on real
    // hardware was 3.7% (see kKvBytesPerTokenPerGiB), and 15% plus a KV term
    // put a 6.8 GB model over the top of an 8 GB card.
    double footprint_margin = 1.08;

    // §6.2 contention prior: 1.0 is perfect fair-share, the physically correct
    // starting point for a saturated GPU.
    double contention_alpha = 1.0;

    // §8: "EWMA half-lives are configuration, not constants". A cluster whose
    // hardware changes should forget at a rate the operator chooses. Expressed
    // in observations, not seconds — what matters is how many jobs a node has
    // run, not how long it has been up.
    double ewma_halflife = 20;

    // Consulted before the built-in table, in order. Lets an operator describe
    // hardware the built-ins do not recognise without a rebuild.
    std::vector<GpuSeed> gpu_seeds;

    static ScoringConfig from_config(const class Config& cfg);
};

// Is this model loaded on this node, as far as this router can tell?
//
// `node.is_resident` is the engine's answer, and it arrives on a poll: the
// agent caches a snapshot on its own loop and the router fetches it on another,
// so the answer can be two intervals behind the load that produced it. The VRAM
// disappears from telemetry immediately, which is how a node comes to look full
// for no visible reason.
//
// Having just served the model there is first-hand evidence, and it is newer
// than any poll. `node_stale_ms` is the horizon this router already uses to
// decide a node's state is too old to act on, so it bounds this too rather than
// introducing a second constant.
//
// Both admission and the cost models need this and must not disagree: if
// admission believes a node can serve without loading while the estimate prices
// a full load, the router admits the warm node and then ranks it as if it were
// cold (D36).
inline bool believed_loaded(const NodeState& node, const std::string& model,
                            const LedgerView& ledger, const ScoringConfig& scoring,
                            int64_t now_ms) {
    if (node.is_resident(model)) return true;
    const int64_t served = ledger.last_served_ms(node.id, model);
    return served > 0 && now_ms - served <= scoring.node_stale_ms;
}

// What admission decided this request would have to evict on a given node.
// Passed into estimate() so that T_evict is computed where every other duration
// is computed, rather than bolted on by the caller (§6.2, D7).
struct EvictionPlan {
    uint64_t bytes = 0;
    bool evicts_warm = false;
};

class ICostModel {
public:
    virtual ~ICostModel() = default;

    virtual const char* name() const = 0;

    // Called under a shared lock. Must not block and must not perform I/O.
    // `now_ms` is a parameter rather than a clock read so that replaying a
    // decision from a trace reproduces it exactly.
    virtual Estimate estimate(const RequestFeatures& req, const NodeState& node,
                              const LedgerView& ledger, const EvictionPlan& evict,
                              int64_t now_ms) const = 0;

    // VRAM this model needs on this node at this context size. Admission
    // depends on it, so it is part of the interface rather than a constant
    // hidden inside the scorer (D2).
    virtual uint64_t footprint_bytes(const std::string& model, const NodeState& node,
                                     uint32_t num_ctx) const = 0;

    // Expected output length, with its own uncertainty. Split out because its
    // error must be tracked separately from timing error (§8).
    virtual OutputPrediction predict_output(const RequestFeatures& req) const = 0;

    // Called once per completed job, under an exclusive lock.
    virtual void observe(const TraceRecord& record) = 0;
};

class IPolicy {
public:
    virtual ~IPolicy() = default;
    virtual const char* name() const = 0;

    // Called under a shared lock. Returns a Decision whose candidate list
    // covers every node, admitted or not: a node that influenced routing
    // without appearing here is a bug (§10).
    virtual Decision select(const RequestFeatures& req,
                            const std::vector<NodeState>& nodes,
                            const LedgerView& ledger, const ICostModel& cost,
                            const ScoringConfig& scoring) const = 0;
};

std::unique_ptr<IPolicy> make_round_robin_policy();
std::unique_ptr<IPolicy> make_warmth_policy();

std::unique_ptr<ICostModel> make_static_cost_model(const ScoringConfig& scoring);

// Phase 2. Seeds every quantity from the static table and replaces each one
// independently as observations arrive (§8).
std::unique_ptr<ICostModel> make_learned_cost_model(const ScoringConfig& scoring);

}  // namespace rf
