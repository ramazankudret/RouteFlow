// RouteFlow — the router's shared state (ARCHITECTURE §4.2.1, D13).
//
// One shared_mutex guards the ledger, the cost model, the active policy and the
// scoring config. Two rules make it reviewable:
//
//   1. No component performs I/O or blocks while the lock is held.
//   2. Scoring and reserving happen under ONE exclusive lock, not two.
//
// Rule 2 refines D13, which described scoring under a shared lock. Scoring
// under a shared lock and then reserving under an exclusive one leaves a gap
// between the two, and that gap is exactly the race the ledger exists to close
// (§6.3): two requests would both score against a state neither had yet
// committed to. Scoring is a few dozen microseconds of arithmetic over a
// handful of nodes, so holding the exclusive lock across it costs nothing worth
// the correctness.
#pragma once

#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "common/json.h"
#include "common/types.h"
#include "router/core/interfaces.h"
#include "router/core/ledger.h"
#include "router/core/registry.h"

namespace rf {

// Everything the dispatcher needs to know after the atomic score-and-reserve
// step, copied out so it can work without the lock.
struct Reservation {
    Decision decision;
    NodeLedger::Token token = NodeLedger::kInvalid;
    bool ok = false;

    std::string node_id;
    bool was_resident = false;
    bool will_load = false;
    uint64_t footprint_bytes = 0;
    double predicted_load_ms = 0;

    // Ledger and telemetry values at the instant of the decision. Recorded
    // rather than re-read at completion, when they would describe a different
    // moment.
    uint32_t inflight_at_dispatch = 0;
    uint32_t concurrent_decoders_at_dispatch = 0;
    bool telemetry_ok = false;
    float gpu_util = 0.f;
    uint64_t vram_free = 0;
    std::string telemetry_backend;
    std::vector<std::string> would_evict;

    std::string policy_name;
    std::string cost_model_name;
};

class RouterState {
public:
    RouterState(NodeRegistry& registry, std::unique_ptr<ICostModel> cost,
                std::unique_ptr<IPolicy> policy, ScoringConfig scoring);

    // Exclusive. Scores every node and, if a winner is admissible, commits the
    // reservation before returning — see the header comment.
    Reservation score_and_reserve(const RequestFeatures& req);

    // Exclusive. Re-scores excluding nodes already tried, for the one permitted
    // pre-first-byte retry (§6.4, D9).
    Reservation score_and_reserve_excluding(const RequestFeatures& req,
                                            const std::vector<std::string>& skip);

    // Exclusive. The model finished loading on the winner: release the VRAM
    // reservation so a concurrent request stops seeing it as pending.
    void note_load_done(NodeLedger::Token token);
    // Exclusive. First token seen: this request now contends for decode (D5).
    void note_decoding(NodeLedger::Token token);

    // Exclusive. Releases the reservation and feeds the cost model. Call once
    // per attempt, including failed ones.
    void complete(NodeLedger::Token token, const TraceRecord& record);

    // Exclusive. Runtime policy switching (§5). Returns false for an unknown
    // name, leaving the active policy untouched.
    bool set_policy(const std::string& name);
    // Exclusive. Same contract as set_policy: unknown name leaves the active
    // model untouched and returns false.
    bool set_cost_model(const std::string& name);

    // Exclusive. Feeds one historical record to the cost model without
    // touching the ledger — §4.2's "rebuilds it from the trace log on start".
    void replay(const TraceRecord& record);
    std::string policy_name() const;
    std::string cost_model_name() const;

    const ScoringConfig& scoring() const { return scoring_; }
    Json stats_json() const;

    // Per-node ledger counts, keyed by node id. Shared lock. The UI shows this
    // rather than NodeState::inflight_reported because it is the number the
    // scheduler actually reasoned about — the engine's own view is advisory and
    // arrives a poll interval late (§6.3).
    Json ledger_counts_json() const;

private:
    Reservation reserve_locked(const RequestFeatures& req,
                               const std::vector<std::string>& skip);

    mutable std::shared_mutex mu_;
    NodeRegistry& registry_;
    NodeLedger ledger_;
    std::unique_ptr<ICostModel> cost_;
    std::unique_ptr<IPolicy> policy_;
    ScoringConfig scoring_;

    uint64_t jobs_dispatched_ = 0;
    uint64_t jobs_no_candidate_ = 0;
};

}  // namespace rf
