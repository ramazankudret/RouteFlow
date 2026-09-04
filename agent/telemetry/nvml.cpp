// NVML telemetry, loaded at runtime.
//
// nvml.h is deliberately NOT included: it ships with the CUDA toolkit, which is
// absent on a Jetson and on any machine that only has a driver. The handful of
// entry points we use have a stable ABI, so they are declared here and resolved
// with dlsym. Nothing links against libnvidia-ml.

#include <dlfcn.h>

#include <cstring>

#include "agent/telemetry/telemetry.h"
#include "common/util.h"

namespace rf {
namespace {

// --- NVML ABI (subset) ------------------------------------------------------

using nvmlDevice_t = void*;
constexpr int NVML_SUCCESS = 0;
constexpr unsigned NVML_TEMPERATURE_GPU = 0;

struct nvmlMemory_t {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

struct nvmlUtilization_t {
    unsigned int gpu;
    unsigned int memory;
};

using FnInit = int (*)();
using FnShutdown = int (*)();
using FnDeviceGetCount = int (*)(unsigned int*);
using FnDeviceGetHandleByIndex = int (*)(unsigned int, nvmlDevice_t*);
using FnDeviceGetName = int (*)(nvmlDevice_t, char*, unsigned int);
using FnDeviceGetUUID = int (*)(nvmlDevice_t, char*, unsigned int);
using FnDeviceGetMemoryInfo = int (*)(nvmlDevice_t, nvmlMemory_t*);
using FnDeviceGetUtilizationRates = int (*)(nvmlDevice_t, nvmlUtilization_t*);
using FnDeviceGetTemperature = int (*)(nvmlDevice_t, unsigned int, unsigned int*);
using FnDeviceGetPowerUsage = int (*)(nvmlDevice_t, unsigned int*);
using FnDeviceGetEnforcedPowerLimit = int (*)(nvmlDevice_t, unsigned int*);

class NvmlTelemetry : public ITelemetry {
public:
    ~NvmlTelemetry() override {
        if (lib_) {
            if (initialised_ && shutdown_) shutdown_();
            ::dlclose(lib_);
        }
    }

    const char* name() const override { return "nvml"; }

    bool probe() override {
        // .so.1 is the driver-provided runtime; the unversioned .so only exists
        // when a development package is installed.
        for (const char* soname : {"libnvidia-ml.so.1", "libnvidia-ml.so"}) {
            lib_ = ::dlopen(soname, RTLD_LAZY | RTLD_LOCAL);
            if (lib_) break;
        }
        if (!lib_) {
            RF_DEBUG("nvml: libnvidia-ml not present (%s)", ::dlerror());
            return false;
        }

        // _v2 entry points where they exist: the v1 symbols are deprecated and
        // absent from some driver branches.
        init_ = sym<FnInit>("nvmlInit_v2", "nvmlInit");
        shutdown_ = sym<FnShutdown>("nvmlShutdown", nullptr);
        count_ = sym<FnDeviceGetCount>("nvmlDeviceGetCount_v2", "nvmlDeviceGetCount");
        handle_ = sym<FnDeviceGetHandleByIndex>("nvmlDeviceGetHandleByIndex_v2",
                                                "nvmlDeviceGetHandleByIndex");
        get_name_ = sym<FnDeviceGetName>("nvmlDeviceGetName", nullptr);
        get_uuid_ = sym<FnDeviceGetUUID>("nvmlDeviceGetUUID", nullptr);
        get_mem_ = sym<FnDeviceGetMemoryInfo>("nvmlDeviceGetMemoryInfo", nullptr);
        get_util_ = sym<FnDeviceGetUtilizationRates>("nvmlDeviceGetUtilizationRates",
                                                     nullptr);
        get_temp_ = sym<FnDeviceGetTemperature>("nvmlDeviceGetTemperature", nullptr);
        get_power_ = sym<FnDeviceGetPowerUsage>("nvmlDeviceGetPowerUsage", nullptr);
        get_cap_ = sym<FnDeviceGetEnforcedPowerLimit>("nvmlDeviceGetEnforcedPowerLimit",
                                                      nullptr);

        // Only these four are load-bearing. The rest are optional signals whose
        // absence omits a term rather than disabling the backend.
        if (!init_ || !count_ || !handle_ || !get_mem_) {
            RF_WARN("nvml: library present but required symbols missing");
            return false;
        }
        if (init_() != NVML_SUCCESS) {
            RF_DEBUG("nvml: nvmlInit failed (no driver or no permission)");
            return false;
        }
        initialised_ = true;

        unsigned int n = 0;
        if (count_(&n) != NVML_SUCCESS || n == 0) {
            RF_DEBUG("nvml: no devices");
            return false;
        }
        device_count_ = n;
        return true;
    }

    bool sample(std::vector<GpuSample>& out) override {
        out.clear();
        for (unsigned int i = 0; i < device_count_; ++i) {
            nvmlDevice_t dev = nullptr;
            if (handle_(i, &dev) != NVML_SUCCESS) continue;

            GpuSample s;
            char buf[128];
            if (get_name_ && get_name_(dev, buf, sizeof buf) == NVML_SUCCESS) {
                buf[sizeof buf - 1] = '\0';
                s.name = buf;
            }
            if (get_uuid_ && get_uuid_(dev, buf, sizeof buf) == NVML_SUCCESS) {
                buf[sizeof buf - 1] = '\0';
                s.uuid = buf;
            }

            nvmlMemory_t mem{};
            if (get_mem_(dev, &mem) != NVML_SUCCESS) continue;  // no VRAM, no candidate
            s.vram_total_bytes = mem.total;
            s.vram_free_bytes = mem.free;

            nvmlUtilization_t util{};
            if (get_util_ && get_util_(dev, &util) == NVML_SUCCESS) {
                s.has_util = true;
                s.util = static_cast<float>(util.gpu) / 100.f;
            }
            unsigned int temp = 0;
            if (get_temp_ && get_temp_(dev, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS) {
                s.has_temperature = true;
                s.temperature_c = static_cast<float>(temp);
            }
            unsigned int mw = 0;
            if (get_power_ && get_power_(dev, &mw) == NVML_SUCCESS) {
                s.has_power = true;
                s.power_watts = static_cast<float>(mw) / 1000.f;
            }
            if (get_cap_ && get_cap_(dev, &mw) == NVML_SUCCESS) {
                s.has_power_cap = true;
                s.power_cap_watts = static_cast<float>(mw) / 1000.f;
            }
            out.push_back(std::move(s));
        }
        return !out.empty();
    }

private:
    template <typename Fn>
    Fn sym(const char* primary, const char* fallback) {
        ::dlerror();
        void* p = ::dlsym(lib_, primary);
        if (!p && fallback) p = ::dlsym(lib_, fallback);
        return reinterpret_cast<Fn>(p);
    }

    void* lib_ = nullptr;
    bool initialised_ = false;
    unsigned int device_count_ = 0;

    FnInit init_ = nullptr;
    FnShutdown shutdown_ = nullptr;
    FnDeviceGetCount count_ = nullptr;
    FnDeviceGetHandleByIndex handle_ = nullptr;
    FnDeviceGetName get_name_ = nullptr;
    FnDeviceGetUUID get_uuid_ = nullptr;
    FnDeviceGetMemoryInfo get_mem_ = nullptr;
    FnDeviceGetUtilizationRates get_util_ = nullptr;
    FnDeviceGetTemperature get_temp_ = nullptr;
    FnDeviceGetPowerUsage get_power_ = nullptr;
    FnDeviceGetEnforcedPowerLimit get_cap_ = nullptr;
};

}  // namespace

std::unique_ptr<ITelemetry> make_nvml_telemetry() {
    return std::unique_ptr<ITelemetry>(new NvmlTelemetry());
}

}  // namespace rf
