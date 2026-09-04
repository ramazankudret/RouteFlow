#include "router/policy/candidates.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "router/core/admission.h"

namespace rf {

std::vector<Candidate> build_candidates(const RequestFeatures& req,
                                        const std::vector<NodeState>& nodes,
                                        const LedgerView& ledger, const ICostModel& cost,
                                        const ScoringConfig& scoring, int64_t now_ms,
                                        const std::vector<std::string>& excluded) {
    std::vector<Candidate> out;
    out.reserve(nodes.size());

    for (const NodeState& node : nodes) {
        Candidate c;
        c.node_id = node.id;
        // State at decision time, recorded for rejected nodes too: without the
        // losers' numbers there is no counterfactual analysis, and the Phase 1
        // result cannot be defended (D10).
        c.vram_free_bytes = node.vram_free_bytes;
        c.gpu_util = node.gpu_util;
        c.telemetry_ok = node.telemetry_ok;
        c.was_resident = node.is_resident(req.model);
        c.inflight = ledger.inflight(node.id);

        const AdmissionResult a =
            admit(req, node, ledger, cost, scoring, now_ms, excluded);
        c.admitted = a.admitted;
        c.reason = a.reason;
        c.would_evict = a.would_evict;

        if (c.admitted) {
            EvictionPlan plan;
            plan.bytes = a.evict_bytes;
            plan.evicts_warm = a.evicts_warm;
            c.est = cost.estimate(req, node, ledger, plan, now_ms);
            if (!node.residency_known) c.est.omitted_terms |= kTermLoad;
        }
        out.push_back(std::move(c));
    }
    return out;
}

void finish_decision(Decision& decision, size_t winner_index) {
    if (winner_index >= decision.candidates.size()) {
        decision.winner_node_id.clear();
        decision.decided_by = "no_candidate";
        decision.margin_ms = 0;
        decision.has_runner_up = false;
        return;
    }

    const Candidate& winner = decision.candidates[winner_index];
    decision.winner_node_id = winner.node_id;

    // Best admitted candidate other than the winner.
    const Candidate* runner_up = nullptr;
    double best_other = std::numeric_limits<double>::max();
    for (size_t i = 0; i < decision.candidates.size(); ++i) {
        if (i == winner_index) continue;
        const Candidate& c = decision.candidates[i];
        if (!c.admitted) continue;
        const double total = c.est.total_ms();
        if (total < best_other) {
            best_other = total;
            runner_up = &c;
        }
    }

    if (!runner_up) {
        decision.has_runner_up = false;
        decision.margin_ms = 0;
        decision.decided_by = "single_candidate";
        return;
    }

    decision.has_runner_up = true;
    decision.margin_ms = best_other - winner.est.total_ms();

    // Inside either candidate's uncertainty the ranking is not distinguishable
    // from noise. The policy still picks the minimum, but the trace and the UI
    // must not present that as a considered decision.
    const double noise = std::max(winner.est.sigma_ms, runner_up->est.sigma_ms);
    if (std::fabs(decision.margin_ms) < noise) {
        decision.decided_by = "within_noise";
        return;
    }

    // Otherwise: the term whose difference explains the most of the gap.
    const double diffs[5] = {
        runner_up->est.t_queue_ms - winner.est.t_queue_ms,
        runner_up->est.t_load_ms - winner.est.t_load_ms,
        runner_up->est.t_prefill_ms - winner.est.t_prefill_ms,
        runner_up->est.t_decode_ms - winner.est.t_decode_ms,
        runner_up->est.t_evict_ms - winner.est.t_evict_ms,
    };
    static const char* kNames[5] = {"t_queue", "t_load", "t_prefill", "t_decode",
                                    "t_evict"};
    int best = 0;
    for (int i = 1; i < 5; ++i)
        if (std::fabs(diffs[i]) > std::fabs(diffs[best])) best = i;
    decision.decided_by = std::fabs(diffs[best]) > 0 ? kNames[best] : "tie";
}

}  // namespace rf
