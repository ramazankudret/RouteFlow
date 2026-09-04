// RouteFlow — local inference engine probes (agent side).
//
// The agent asks the engine two questions that are not the same question:
// which models exist on disk, and which are resident in VRAM right now. Keeping
// them apart is the core of the project (ARCHITECTURE §4.1), so the report
// carries both plus a flag for engines that cannot answer the second one.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/types.h"

namespace rf {

struct EngineReport {
    bool healthy = false;
    std::vector<std::string> models_on_disk;
    std::vector<std::pair<std::string, uint64_t>> model_disk_bytes;
    std::vector<ResidentModel> models_resident;
    // False when the engine exposes no residency endpoint. The router then
    // omits T_load for this node rather than assuming every model is cold.
    bool residency_known = true;
    uint32_t inflight = 0;  // 0 when the engine does not report it
};

class IEngineProbe {
public:
    virtual ~IEngineProbe() = default;
    virtual EngineKind kind() const = 0;
    virtual const char* name() const = 0;
    // Never throws; on failure sets healthy=false and returns false so the node
    // stays visible as an unhealthy candidate rather than disappearing.
    virtual bool poll(EngineReport& out) = 0;
};

std::unique_ptr<IEngineProbe> make_ollama_probe(const std::string& host, uint16_t port,
                                                int timeout_ms);
std::unique_ptr<IEngineProbe> make_lmstudio_probe(const std::string& host, uint16_t port,
                                                  int timeout_ms);

}  // namespace rf
