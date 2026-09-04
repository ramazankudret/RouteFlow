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
    double footprint_margin = 1.15;

    // §6.2 contention prior: 1.0 is perfect fair-share, the physically correct
    // starting point for a saturated GPU.
    double contention_alpha = 1.0;

    static ScoringConfig from_config(const class Config& cfg);
};

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

std::unique_ptr<ICostModel> make_static_cost_model(const ScoringConfig& scoring);

}  // namespace rf
