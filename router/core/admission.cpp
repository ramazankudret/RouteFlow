#include "router/core/admission.h"

#include <algorithm>

#include "common/util.h"

namespace rf {

AdmissionResult admit(const RequestFeatures& req, const NodeState& node,
                      const LedgerView& ledger, const ICostModel& cost,
                      const ScoringConfig& scoring, int64_t now_ms,
                      const std::vector<std::string>& excluded_nodes) {
    AdmissionResult out;

    auto reject = [&out](AdmitReason reason) {
        out.reason = reason;
        out.admitted = false;
        return out;
    };

    if (std::find(excluded_nodes.begin(), excluded_nodes.end(), node.id) !=
        excluded_nodes.end())
        return reject(AdmitReason::Excluded);

    if (!node.engine_healthy || node.engine == EngineKind::None)
        return reject(AdmitReason::EngineDown);

    // A node we have not heard from recently is not a candidate: its resident
    // set and free VRAM are the inputs to every other filter below, and acting
    // on a stale snapshot is how a scheduler sends work to a machine that is
    // already gone.
    if (node.sampled_at_ms > 0 && now_ms - node.sampled_at_ms > scoring.node_stale_ms)
        return reject(AdmitReason::NodeStale);

    if (!node.has_on_disk(req.model)) return reject(AdmitReason::ModelMissing);

    out.footprint_bytes = cost.footprint_bytes(req.model, node, req.num_ctx);

    // Already resident, or already being loaded by us: no VRAM to find.
    if (node.is_resident(req.model) || ledger.pending(node.id, req.model) != nullptr) {
        out.admitted = true;
        return out;
    }

    // A node that reports no VRAM total has no telemetry to constrain against.
    // Admitting it unconditionally would let a 70B model onto a Raspberry Pi;
    // rejecting it would make every no-telemetry node useless. The engine is
    // the authority in that case, so we admit and let it refuse — recorded, so
    // the failure is attributable.
    if (node.vram_total_bytes == 0) {
        out.admitted = true;
        return out;
    }

    const uint64_t reserved = ledger.reserved_vram_bytes(node.id);
    const uint64_t free_effective =
        node.vram_free_bytes > reserved ? node.vram_free_bytes - reserved : 0;

    if (free_effective >= out.footprint_bytes) {
        out.admitted = true;
        return out;
    }

    // Not enough free VRAM. Rev 1 stopped here and rejected the node; that
    // wrongly eliminates any machine holding an idle model it could drop (D7).
    // Evict least-recently-used first, skipping models this router is currently
    // generating with.
    std::vector<const ResidentModel*> evictable;
    for (const auto& m : node.models_resident) {
        if (m.name == req.model) continue;
        if (ledger.model_busy(node.id, m.name)) continue;
        evictable.push_back(&m);
    }
    std::sort(evictable.begin(), evictable.end(),
              [](const ResidentModel* a, const ResidentModel* b) {
                  return a->last_used_ms < b->last_used_ms;
              });

    uint64_t reclaimed = free_effective;
    for (const ResidentModel* m : evictable) {
        if (reclaimed >= out.footprint_bytes) break;
        reclaimed += m->vram_bytes;
        out.would_evict.push_back(m->name);
        out.evict_bytes += m->vram_bytes;
        if (m->last_used_ms > 0 && now_ms - m->last_used_ms <= scoring.warm_window_ms)
            out.evicts_warm = true;
    }

    if (reclaimed < out.footprint_bytes) {
        out.would_evict.clear();
        out.evict_bytes = 0;
        out.evicts_warm = false;
        return reject(AdmitReason::InsufficientVram);
    }

    out.admitted = true;
    return out;
}

}  // namespace rf
