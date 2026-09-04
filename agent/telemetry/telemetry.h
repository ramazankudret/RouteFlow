// RouteFlow — GPU telemetry backends (ARCHITECTURE §4.1.1, D3).
//
// This is the only part of the tree allowed to carry platform-specific code.
// A missing backend degrades the score by omitting terms; it never crashes the
// agent and never reports a fabricated zero.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rf {

struct GpuSample {
    std::string name;
    std::string uuid;  // stable across reboots where the backend exposes one

    uint64_t vram_total_bytes = 0;
    uint64_t vram_free_bytes = 0;

    // Availability is per field: Tegra exposes utilization and temperature but
    // often no enforced power cap, and reporting 0 W would be a lie the
    // scheduler would then act on.
    bool has_util = false;
    float util = 0.f;  // 0..1
    bool has_temperature = false;
    float temperature_c = 0.f;
    bool has_power = false;
    float power_watts = 0.f;
    bool has_power_cap = false;
    float power_cap_watts = 0.f;
};

class ITelemetry {
public:
    virtual ~ITelemetry() = default;

    virtual const char* name() const = 0;  // "nvml" | "tegra" | "null"
    // True if this backend can actually read the hardware on this host.
    // Called once at startup; must not throw and must clean up after itself.
    virtual bool probe() = 0;
    virtual bool sample(std::vector<GpuSample>& out) = 0;
};

std::unique_ptr<ITelemetry> make_nvml_telemetry();
std::unique_ptr<ITelemetry> make_tegra_telemetry();
std::unique_ptr<ITelemetry> make_null_telemetry();

// Probes nvml, then tegra, then null. `forced` pins a specific backend by name
// and returns null (the pointer) if that backend does not probe, so that
// --telemetry nvml on a machine without NVML is an error rather than a silent
// downgrade to fabricated data.
std::unique_ptr<ITelemetry> select_telemetry(const std::string& forced = std::string());

}  // namespace rf
