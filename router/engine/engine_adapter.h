// RouteFlow — engine adapters (ARCHITECTURE §5, Phase 3).
//
// The one component that *writes* to a node. Everything else in the router
// observes and decides; this is where a decision becomes a model moving into or
// out of VRAM, so it is deliberately the narrowest interface in the tree: load
// one model, drop one model, and say whether it worked.
//
// Two deviations from the §5 sketch, both because the sketch predates the
// agent:
//
//   `resident()` is gone. §5 gave the adapter a method to list resident models,
//   but NodeState already carries that from the agent's own poll, and a second
//   path to the same fact is a second thing that can disagree with the first.
//
//   Calls go to the node's dispatch endpoint, which is the agent (D18), not to
//   the engine directly. The agent already proxies these paths for inference;
//   placement rides the same road rather than opening a second one to a port
//   that is supposed to stay on loopback.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/types.h"

namespace rf {

struct EngineTarget {
    std::string host;
    uint16_t port = 0;
    std::string token;   // bearer for the agent, empty when unauthenticated
    EngineKind kind = EngineKind::None;
};

class IEngineAdapter {
public:
    virtual ~IEngineAdapter() = default;
    virtual const char* name() const = 0;

    // Bring a model into VRAM without generating anything. Blocks for the load,
    // which can be minutes, so callers run it off the request path.
    virtual bool preload(const EngineTarget& target, const std::string& model,
                         int timeout_ms, std::string* err) = 0;

    // Drop a model from VRAM now, rather than when the engine's own timer says
    // so. This is the half that makes placement a decision rather than a hint:
    // without it the manager can only add, and a full node stays full.
    virtual bool evict(const EngineTarget& target, const std::string& model,
                       int timeout_ms, std::string* err) = 0;
};

// Ollama, and anything speaking its API — which includes the simulated node, on
// purpose, so placement is exercised by the bench exactly as it would be on
// real hardware.
std::unique_ptr<IEngineAdapter> make_ollama_adapter();

// Chooses by EngineKind. Returns nullptr for an engine RouteFlow cannot place
// on, which is not an error: such a node still routes, it just manages its own
// residency.
std::unique_ptr<IEngineAdapter> make_engine_adapter(EngineKind kind);

}  // namespace rf
