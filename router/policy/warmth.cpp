// Warmth ("warmth-v1") — the policy the project exists to test (§6.2).
//
// It picks the admitted candidate with the lowest predicted completion time.
// That is the whole policy: there is no warmth bonus, no heuristic tiebreak,
// no hand-tuned weight. The claim being tested is that *predicting completion
// time properly* — with model load priced explicitly, in seconds, comparable
// against decode and queue — is enough to beat queue-depth routing on
// heterogeneous hardware.
//
// Keeping the policy this thin is deliberate. It means a Phase 1 loss can only
// be blamed on the cost model or on the premise, never on a fudge factor buried
// here, and it means every number behind the decision is already in the
// Decision breakdown where the UI and the harness can see it.

#include <cstddef>
#include <limits>

#include "common/util.h"
#include "router/core/interfaces.h"
#include "router/policy/candidates.h"

namespace rf {
namespace {

class WarmthPolicy : public IPolicy {
public:
    const char* name() const override { return "warmth-v1"; }

    Decision select(const RequestFeatures& req, const std::vector<NodeState>& nodes,
                    const LedgerView& ledger, const ICostModel& cost,
                    const ScoringConfig& scoring,
                    const std::vector<std::string>& excluded) const override {
        Decision d;
        d.policy_name = name();
        d.candidates =
            build_candidates(req, nodes, ledger, cost, scoring, now_ms(), excluded);

        size_t winner = static_cast<size_t>(-1);
        double best = std::numeric_limits<double>::max();
        for (size_t i = 0; i < d.candidates.size(); ++i) {
            const Candidate& c = d.candidates[i];
            if (!c.admitted) continue;
            const double total = c.est.total_ms();
            // Strictly less than: on an exact tie the earlier node wins, which
            // makes the choice a function of registry order rather than of
            // floating-point noise. Registry order is config order, so a tied
            // run reproduces.
            if (total < best) {
                best = total;
                winner = i;
            }
        }

        finish_decision(d, winner);
        return d;
    }
};

}  // namespace

std::unique_ptr<IPolicy> make_warmth_policy() {
    return std::unique_ptr<IPolicy>(new WarmthPolicy());
}

}  // namespace rf
