// RouteFlow — admission filters (ARCHITECTURE §6.1).
//
// Admission is deliberately NOT part of IPolicy. Every policy applies exactly
// the same hard filters, because a baseline that admits a different set of
// nodes is not a baseline — the Phase 1 comparison would be measuring two
// things at once.
#pragma once

#include <string>
#include <vector>

#include "common/types.h"
#include "router/core/interfaces.h"

namespace rf {

struct AdmissionResult {
    AdmitReason reason = AdmitReason::Ok;
    bool admitted = false;
    uint64_t footprint_bytes = 0;
    // Models that would have to be evicted to fit. Priced by T_evict (§6.2)
    // and recorded in the Decision so the cost is never invisible.
    std::vector<std::string> would_evict;
    uint64_t evict_bytes = 0;
    bool evicts_warm = false;  // at least one victim was used within warm_window
};

// `now_ms` is passed in rather than read from the clock so that a decision is
// reproducible when replayed from a trace.
AdmissionResult admit(const RequestFeatures& req, const NodeState& node,
                      const LedgerView& ledger, const ICostModel& cost,
                      const ScoringConfig& scoring, int64_t now_ms,
                      const std::vector<std::string>& excluded_nodes);

}  // namespace rf
