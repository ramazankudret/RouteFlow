// Jetson (Tegra) telemetry from sysfs.
//
// NVML does not report the integrated GPU on Tegra, which is exactly where the
// thermal and power terms are supposed to earn their keep (D3). Everything here
// comes from sysfs, so it needs no NVIDIA userspace at all.
//
// Sysfs layout differs across JetPack releases, so each signal is probed over a
// list of known paths and simply omitted if none of them exists.

#include <dirent.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "agent/telemetry/telemetry.h"
#include "common/util.h"

namespace rf {
namespace {

bool read_trimmed(const std::string& path, std::string& out) {
    if (!read_file(path, out)) return false;
    out = trim(out);
    return !out.empty();
}

bool read_long(const std::string& path, long& out) {
    std::string text;
    if (!read_trimmed(path, text)) return false;
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str()) return false;
    out = v;
    return true;
}

std::string first_existing(const std::vector<std::string>& paths) {
    std::string ignored;
    for (const auto& p : paths)
        if (read_file(p, ignored)) return p;
    return std::string();
}

// Total/available memory in bytes from /proc/meminfo. Tegra has unified memory:
// "VRAM" is system RAM, and MemAvailable is the honest figure for what a model
// can actually claim.
bool read_meminfo(uint64_t& total_bytes, uint64_t& available_bytes) {
    std::string text;
    if (!read_file("/proc/meminfo", text)) return false;
    total_bytes = available_bytes = 0;
    for (const auto& line : split(text, '\n')) {
        const char* key = nullptr;
        uint64_t* dest = nullptr;
        if (starts_with(line, "MemTotal:")) { key = "MemTotal:"; dest = &total_bytes; }
        else if (starts_with(line, "MemAvailable:")) { key = "MemAvailable:"; dest = &available_bytes; }
        if (!dest) continue;
        *dest = std::strtoull(line.c_str() + std::strlen(key), nullptr, 10) * 1024ULL;
    }
    return total_bytes > 0;
}

// Thermal zone whose type matches one of the GPU zone names used by JetPack.
std::string find_gpu_thermal_zone() {
    DIR* dir = ::opendir("/sys/devices/virtual/thermal");
    if (!dir) return std::string();
    std::string found;
    while (dirent* ent = ::readdir(dir)) {
        if (!starts_with(ent->d_name, "thermal_zone")) continue;
        const std::string base = std::string("/sys/devices/virtual/thermal/") + ent->d_name;
        std::string type;
        if (!read_trimmed(base + "/type", type)) continue;
        const std::string t = lower(type);
        if (t.find("gpu") != std::string::npos) { found = base + "/temp"; break; }
        if (found.empty() && (t.find("soc") != std::string::npos ||
                              t.find("tj") != std::string::npos)) {
            found = base + "/temp";  // fallback; keep looking for a real GPU zone
        }
    }
    ::closedir(dir);
    return found;
}

// INA3221 rail whose label mentions the GPU. Path shape varies by carrier board.
void find_power_rail(std::string& power_path, std::string& cap_path) {
    static const char* kRoots[] = {
        "/sys/bus/i2c/drivers/ina3221x",
        "/sys/bus/i2c/drivers/ina3221",
    };
    for (const char* root : kRoots) {
        DIR* dir = ::opendir(root);
        if (!dir) continue;
        while (dirent* ent = ::readdir(dir)) {
            if (ent->d_name[0] == '.') continue;
            const std::string dev = std::string(root) + "/" + ent->d_name;
            for (int ch = 0; ch < 3; ++ch) {
                const std::string idx = std::to_string(ch + 1);
                std::string label;
                if (!read_trimmed(dev + "/hwmon/hwmon0/in" + idx + "_label", label) &&
                    !read_trimmed(dev + "/iio:device0/rail_name_" + std::to_string(ch),
                                  label))
                    continue;
                if (lower(label).find("gpu") == std::string::npos) continue;
                power_path = first_existing({dev + "/hwmon/hwmon0/curr" + idx + "_input",
                                             dev + "/iio:device0/in_power" +
                                                 std::to_string(ch) + "_input"});
                cap_path = first_existing({dev + "/iio:device0/crit_current_limit_" +
                                           std::to_string(ch)});
                if (!power_path.empty()) { ::closedir(dir); return; }
            }
        }
        ::closedir(dir);
    }
}

class TegraTelemetry : public ITelemetry {
public:
    const char* name() const override { return "tegra"; }

    bool probe() override {
        // The GPU load node is the one signal that actually identifies a Tegra.
        load_path_ = first_existing({
            "/sys/devices/gpu.0/load",
            "/sys/devices/platform/gpu.0/load",
            "/sys/devices/platform/17000000.gpu/load",
        });
        if (load_path_.empty()) {
            RF_DEBUG("tegra: no gpu load node; not a Tegra host");
            return false;
        }
        uint64_t total = 0, avail = 0;
        if (!read_meminfo(total, avail)) {
            RF_WARN("tegra: /proc/meminfo unreadable");
            return false;
        }

        temp_path_ = find_gpu_thermal_zone();
        find_power_rail(power_path_, cap_path_);

        std::string model;
        if (read_trimmed("/proc/device-tree/model", model)) {
            // device-tree strings are NUL-terminated; trim() will not remove it.
            model.erase(std::find(model.begin(), model.end(), '\0'), model.end());
            device_name_ = model;
        } else {
            device_name_ = "Tegra integrated GPU";
        }
        RF_INFO("tegra: load=%s temp=%s power=%s", load_path_.c_str(),
                temp_path_.empty() ? "-" : temp_path_.c_str(),
                power_path_.empty() ? "-" : power_path_.c_str());
        return true;
    }

    bool sample(std::vector<GpuSample>& out) override {
        out.clear();
        GpuSample s;
        s.name = device_name_;

        uint64_t total = 0, avail = 0;
        if (!read_meminfo(total, avail)) return false;
        s.vram_total_bytes = total;
        s.vram_free_bytes = avail;

        long load = 0;
        if (read_long(load_path_, load)) {  // per-mille
            s.has_util = true;
            s.util = static_cast<float>(load) / 1000.f;
            if (s.util > 1.f) s.util = 1.f;
        }
        long millicelsius = 0;
        if (!temp_path_.empty() && read_long(temp_path_, millicelsius)) {
            s.has_temperature = true;
            s.temperature_c = static_cast<float>(millicelsius) / 1000.f;
        }
        long milliwatts = 0;
        if (!power_path_.empty() && read_long(power_path_, milliwatts)) {
            s.has_power = true;
            s.power_watts = static_cast<float>(milliwatts) / 1000.f;
        }
        long cap_mw = 0;
        if (!cap_path_.empty() && read_long(cap_path_, cap_mw) && cap_mw > 0) {
            s.has_power_cap = true;
            s.power_cap_watts = static_cast<float>(cap_mw) / 1000.f;
        }
        out.push_back(std::move(s));
        return true;
    }

private:
    std::string device_name_;
    std::string load_path_;
    std::string temp_path_;
    std::string power_path_;
    std::string cap_path_;
};

}  // namespace

std::unique_ptr<ITelemetry> make_tegra_telemetry() {
    return std::unique_ptr<ITelemetry>(new TegraTelemetry());
}

}  // namespace rf
