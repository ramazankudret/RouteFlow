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

    // Being wrong here costs a reload, which is a slow reply. Being wrong the
    // other way is a 503 for a request the cluster could serve (D36).
    if (believed_loaded(node, req.model, ledger, scoring, now_ms)) {
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

    // Two ways a node can have no room, and for a long time this only knew
    // about one. An engine holding as many models as it will keep is full in
    // the currency it actually counts in, however many free bytes the card
    // reports -- so loading here evicts something whether we plan for it or
    // not, and an unplanned eviction is one T_evict never prices and the trace
    // never records (D38).
    const bool crowded = at_residency_limit(node, req.model);
    if (free_effective >= out.footprint_bytes && !crowded) {
        out.admitted = true;
        return out;
    }

    // Rev 1 stopped here and rejected the node; that wrongly eliminates any
    // machine holding an idle model it could drop (D7). Evict
    // least-recently-used first, skipping models this router is currently
    // generating with.
    //
    // Ollama does not say when a model was last used, only when it expires, so
    // `last_used_ms` arrives as 0 from every real engine -- which made the sort
    // below arbitrary and `evicts_warm` permanently false. T_evict was
    // therefore zero on every real decision this project has ever measured: the
    // externality D7 exists to price was live in simulation and dead on
    // hardware. The router does know the answer, because it knows what it
    // dispatched, so it fills it in from the ledger (D38).
    struct Evictable {
        const ResidentModel* model;
        int64_t last_used_ms;
    };
    std::vector<Evictable> evictable;
    for (const auto& m : node.models_resident) {
        if (m.name == req.model) continue;
        if (ledger.model_busy(node.id, m.name)) continue;
        const int64_t used = m.last_used_ms > 0 ? m.last_used_ms
                                                : ledger.last_served_ms(node.id, m.name);
        evictable.push_back({&m, used});
    }
    std::sort(evictable.begin(), evictable.end(),
              [](const Evictable& a, const Evictable& b) {
                  return a.last_used_ms < b.last_used_ms;
              });

    // Crowding is satisfied by dropping one model; a byte shortfall is
    // satisfied by dropping enough of them. Both can apply at once, so the
    // loop runs until neither does.
    uint64_t reclaimed = free_effective;
    size_t resident_after = node.models_resident.size();
    const size_t room_for =
        node.models_resident_limit > 0 ? node.models_resident_limit - 1 : resident_after;
    for (const Evictable& v : evictable) {
        if (reclaimed >= out.footprint_bytes && resident_after <= room_for) break;
        reclaimed += v.model->vram_bytes;
        --resident_after;
        out.would_evict.push_back(v.model->name);
        out.evict_bytes += v.model->vram_bytes;
        if (v.last_used_ms > 0 && now_ms - v.last_used_ms <= scoring.warm_window_ms)
            out.evicts_warm = true;
    }

    if (reclaimed < out.footprint_bytes || resident_after > room_for) {
        out.would_evict.clear();
        out.evict_bytes = 0;
        out.evicts_warm = false;
        // Every candidate resident model is busy serving this router, so the
        // engine has nothing it can drop for us right now.
        return reject(AdmitReason::InsufficientVram);
    }

    out.admitted = true;
    return out;
}

}  // namespace rf
