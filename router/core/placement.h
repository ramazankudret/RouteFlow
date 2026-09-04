// RouteFlow — placement manager (ARCHITECTURE §9 Phase 3).
//
// The only component that changes the cluster rather than describing it. It
// watches which models are actually being requested where, and moves them into
// VRAM ahead of the next request instead of letting each cold start pay for
// itself.
//
// Reactive only. §9 defers predictive placement — learning which model follows
// which and loading ahead of the request — to Phase 4 and does not commit to
// it, because a single cluster accumulates transition data slowly and beating
// LRU with it is unproven. This manager does the thing that is obviously true
// first: a model that has been asked for repeatedly on a node will be asked for
// again.
//
// Two design facts that shape everything below:
//
//   **Eviction is instant; loading is not.** Dropping a model from VRAM costs
//   no measurable time, so pre-evicting a stale model saves nothing — the next
//   request would have evicted it just as fast at dispatch. All the value is in
//   preloading, and eviction appears here only as a means of making room for
//   one. §9's "evict the stale" is therefore implemented as "evict the stale
//   when something better wants the space", not as a background tidy.
//
//   **A preload is real work on a real slot.** It goes down the same path as
//   inference and occupies the engine while it runs. Preloading onto a busy
//   node steals capacity from the requests it was meant to help, which is
//   precisely the p95 regression the Phase 3 exit criterion guards against. The
//   manager therefore acts only on idle nodes, one action at a time.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common/json.h"
#include "common/types.h"
#include "router/core/registry.h"
#include "router/core/router_state.h"
#include "router/engine/engine_adapter.h"

namespace rf {

struct PlacementConfig {
    // Off by default. Phase 3 is opt-in until it has been measured against the
    // baseline, and the baseline is this being off.
    bool enabled = false;

    int interval_ms = 5000;        // how often placement is reconsidered
    int64_t window_ms = 120000;    // demand is counted over this window
    uint32_t min_demand = 2;       // requests before a model is worth preloading
    // How much hotter an arrival must be than the resident it displaces. 1.0
    // would swap on any tie and thrash; higher is more conservative. This
    // replaced an absolute staleness cutoff, which made the manager inert under
    // pressure — nothing is ever stale during a busy period, so no eviction was
    // ever allowed and no preload needing room could happen.
    double evict_margin = 1.5;
    int preload_timeout_ms = 600000;  // a cold load can take minutes

    static PlacementConfig from_config(const class Config& cfg);
};

class PlacementManager {
public:
    PlacementManager(NodeRegistry& registry, RouterState& state,
                     PlacementConfig config, std::string node_token);
    ~PlacementManager();

    // Every completed job is a demand signal. Called from the trace observer,
    // off the request path.
    void observe(const TraceRecord& record);

    void start();
    void stop();

    Json stats_json() const;

private:
    struct Demand {
        uint32_t count = 0;
        int64_t last_ms = 0;
    };

    void loop();
    void consider(const NodeState& node);

    NodeRegistry& registry_;
    RouterState& state_;
    PlacementConfig config_;
    std::string node_token_;
    std::unique_ptr<IEngineAdapter> ollama_;

    mutable std::mutex mu_;
    std::map<std::string, Demand> demand_;  // node|model
    uint64_t preloads_ = 0;
    uint64_t evictions_ = 0;
    uint64_t failures_ = 0;
    uint64_t skipped_busy_ = 0;
    std::string last_action_;

    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace rf
