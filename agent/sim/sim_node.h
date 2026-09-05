// RouteFlow — simulated node (ARCHITECTURE §4.1.2, D16).
//
// Serves the same /state and the same chat endpoints as a real node, but the
// work is modelled rather than performed: a declared load bandwidth, prefill and
// decode rate per model, a VRAM budget with real eviction, and contention that
// slows concurrent decoders exactly as §6.2 says a GPU does.
//
// The purpose is a *controlled* comparison. The Phase 1 exit criterion asks
// whether Warmth beats RoundRobin on a heterogeneous cluster; run against live
// hardware with uncontrolled thermals and background load, that question cannot
// be answered cleanly. The router is never told which nodes are simulated.
#pragma once

#include <memory>
#include <string>

#include "agent/engine/engine_probe.h"
#include "common/http.h"
#include "common/json.h"

namespace rf {

struct SimModelProfile {
    std::string name;
    uint64_t disk_bytes = 0;
    uint64_t footprint_bytes = 0;      // VRAM when resident
    double prefill_tokens_per_ms = 1;  // prompt processing rate
    double decode_tokens_per_ms = 0.02;
    // How long this model's replies are when the caller states no cap, which is
    // what most agent traffic does. Zero leaves the flat fallback in place.
    uint32_t output_tokens_min = 0;
    uint32_t output_tokens_max = 0;
};

struct SimProfile {
    std::string id;
    std::string gpu_name = "simulated GPU";
    uint64_t vram_total_bytes = 8ULL * 1024 * 1024 * 1024;
    uint32_t engine_slots = 1;
    double load_bandwidth_bytes_per_ms = 1'000'000;  // ~1 GB/s
    double contention_alpha = 1.0;                   // §6.2 fair-share prior
    double jitter = 0.05;                            // fractional, 1-sigma
    uint64_t seed = 1;
    double idle_power_watts = 0;   // 0 => report no power signal
    double busy_power_watts = 0;
    double power_cap_watts = 0;
    double temperature_c = 0;
    std::vector<SimModelProfile> models;
    std::vector<std::string> resident_at_start;

    static bool from_json(const Json& j, SimProfile& out, std::string* err);
};

class SimNode {
public:
    explicit SimNode(SimProfile profile);
    ~SimNode();

    const SimProfile& profile() const;

    // The /state view, in the same shape a real probe produces.
    EngineReport report() const;
    uint64_t vram_free_bytes() const;
    uint32_t inflight() const;
    bool has_power_signal() const;
    float power_watts() const;
    float gpu_util() const;

    // Engine endpoints. Handles the OpenAI (/v1/chat/completions) and Ollama
    // (/api/chat, /api/generate) request shapes, streaming or buffered.
    // Returns false if the path is not one it serves.
    bool handle(const http::Request& req, http::Responder& res);

private:
    // Ollama's load/unload call: /api/generate, empty prompt, keep_alive.
    void handle_placement(const std::string& model, const struct SimModelProfile& mp,
                          bool unload, http::Responder& res);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rf
