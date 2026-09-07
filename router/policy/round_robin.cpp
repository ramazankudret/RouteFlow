// RoundRobin ("roundrobin-v1") — the Phase 1 baseline (ARCHITECTURE §9).
//
// Rotates through the admitted nodes and ignores every prediction. That is the
// point: it is the control the Warmth policy has to beat, and if Warmth cannot
// beat *this* on a heterogeneous cluster then the premise of the project is
// wrong and we want to know immediately.
//
// It still records the full per-candidate estimate, because admission and
// estimation are shared (see policy/candidates.h). A baseline that recorded no
// prediction would make prediction error incomparable between the two runs,
// and a baseline that admitted a different set of nodes would be measuring two
// changes at once.

#include <atomic>

#include "common/util.h"
#include "router/core/interfaces.h"
#include "router/policy/candidates.h"

namespace rf {
namespace {

class RoundRobinPolicy : public IPolicy {
public:
    const char* name() const override { return "roundrobin-v1"; }

    Decision select(const RequestFeatures& req, const std::vector<NodeState>& nodes,
                    const LedgerView& ledger, const ICostModel& cost,
                    const ScoringConfig& scoring,
                    const std::vector<std::string>& excluded) const override {
        Decision d;
        d.policy_name = name();
        d.candidates =
            build_candidates(req, nodes, ledger, cost, scoring, now_ms(), excluded);

        // Rotate over the admitted subset. `nodes` arrives in registry order,
        // which is the config order and therefore stable across runs — the
        // baseline has to be reproducible to be a baseline.
        size_t winner = decision_npos;
        const size_t n = d.candidates.size();
        if (n > 0) {
            const size_t start = cursor_.fetch_add(1, std::memory_order_relaxed) % n;
            for (size_t k = 0; k < n; ++k) {
                const size_t i = (start + k) % n;
                if (d.candidates[i].admitted) {
                    winner = i;
                    break;
                }
            }
        }

        finish_decision(d, winner);
        // Whatever term happens to differ, this policy did not consult it.
        // Recording "t_load" here would misattribute the choice in the trace
        // and in the UI's decision panel.
        if (!d.winner_node_id.empty()) d.decided_by = "round_robin";
        return d;
    }

private:
    static constexpr size_t decision_npos = static_cast<size_t>(-1);

    mutable std::atomic<size_t> cursor_{0};
};

}  // namespace

std::unique_ptr<IPolicy> make_round_robin_policy() {
    return std::unique_ptr<IPolicy>(new RoundRobinPolicy());
}

}  // namespace rf
