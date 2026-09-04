#include "agent/telemetry/telemetry.h"

#include "common/util.h"

namespace rf {

std::unique_ptr<ITelemetry> select_telemetry(const std::string& forced) {
    struct Entry {
        const char* name;
        std::unique_ptr<ITelemetry> (*make)();
    };
    // Probe order matters: NVML before Tegra, because a Jetson with a discrete
    // card attached should use the real thing.
    static const Entry kBackends[] = {
        {"nvml", &make_nvml_telemetry},
        {"tegra", &make_tegra_telemetry},
        {"null", &make_null_telemetry},
    };

    if (!forced.empty()) {
        for (const auto& e : kBackends) {
            if (forced != e.name) continue;
            auto backend = e.make();
            if (backend->probe()) return backend;
            // Explicitly requested and unavailable is an error, not a downgrade:
            // silently falling back would produce a node whose scores omit the
            // very terms the operator asked for.
            RF_ERROR("telemetry backend '%s' was requested but did not probe",
                     forced.c_str());
            return nullptr;
        }
        RF_ERROR("unknown telemetry backend '%s' (nvml|tegra|null)", forced.c_str());
        return nullptr;
    }

    for (const auto& e : kBackends) {
        auto backend = e.make();
        if (!backend->probe()) continue;
        if (std::string(e.name) == "null")
            RF_WARN("no GPU telemetry available; utilization and thermal terms "
                    "will be omitted from scoring on this node");
        else
            RF_INFO("telemetry backend: %s", e.name);
        return backend;
    }
    return nullptr;  // unreachable: null always probes
}

}  // namespace rf
