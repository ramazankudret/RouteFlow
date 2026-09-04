// Shared candidate construction for every policy.
//
// Admission and estimation are identical across policies by construction: a
// baseline that admits a different set of nodes, or that records no prediction,
// is not a baseline — the Phase 1 comparison would be measuring two changes at
// once, and prediction error would not be comparable between the two runs.
//
// What a policy chooses is *which* admitted candidate wins. That is all.
#pragma once

#include <string>
#include <vector>

#include "common/types.h"
#include "router/core/interfaces.h"

namespace rf {

std::vector<Candidate> build_candidates(const RequestFeatures& req,
                                        const std::vector<NodeState>& nodes,
                                        const LedgerView& ledger, const ICostModel& cost,
                                        const ScoringConfig& scoring, int64_t now_ms,
                                        const std::vector<std::string>& excluded);

// Fills winner_node_id, decided_by and margin_ms. `winner` may be npos, which
// records a decision with no admissible candidate rather than an empty object.
//
// decided_by is the term with the largest winner-vs-runner-up gap, or
// "within_noise" when the margin is inside either candidate's sigma: a
// scheduler that cannot tell two options apart should say so rather than
// present a coin flip as a calculation (§6.2, D8).
void finish_decision(Decision& decision, size_t winner_index);

}  // namespace rf
