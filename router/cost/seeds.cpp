#include "router/cost/seeds.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>

#include "common/util.h"

namespace rf {
namespace {

constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

// Built-in hardware seeds, consulted after any operator-supplied ones.
// `mem_bw` is device memory bandwidth in bytes/ms and drives decode; `load_bw`
// is the storage-to-VRAM path and drives T_load. They are different paths and
// do not track each other: a Jetson loads comparatively fast, because unified
// memory means a load never crosses PCIe, while decoding comparatively slowly.
//
// The patterns are deliberately specific. An earlier version keyed on "rtx 40"
// and "rtx 20", which is wrong in a way that matters: bandwidth varies more
// *within* a generation than between them (a 4060 Laptop is 272 GB/s, a 4090
// is 1008), so a generation-wide entry silently hands a laptop part a desktop
// flagship's throughput — and a cost model that cannot tell a strong node from
// a weak one has nothing left to rank on but warmth.
struct GpuSeedRow {
    const char* match;
    double mem_bw;
    double load_bw;
    double prefill_ratio;
};

const GpuSeedRow kGpuSeeds[] = {
    // Jetson / Tegra: unified LPDDR.
    {"agx orin",      205e6, 1.5e6, 30},
    {"orin nx",       102e6, 1.2e6, 30},
    {"orin nano",      68e6, 1.0e6, 30},
    {"orin",          102e6, 1.2e6, 30},
    {"xavier",         59e6, 0.8e6, 30},
    {"jetson",         68e6, 1.0e6, 30},
    // Specific parts, before the generation fallbacks.
    {"4060 laptop",   272e6, 0.5e6, 45},   // measured on this project's machine
    {"4090",         1008e6, 1.0e6, 50},
    {"4080",          717e6, 0.9e6, 50},
    {"4070",          504e6, 0.7e6, 45},
    {"3090",          936e6, 0.9e6, 45},
    {"3080",          760e6, 0.9e6, 45},
    {"3060",          360e6, 0.6e6, 45},
    {"2050",          112e6, 0.4e6, 40},
    {"a100",         1555e6, 2.0e6, 60},
    {"h100",         3350e6, 3.0e6, 60},
    // Generation fallbacks: the conservative end of each.
    {"rtx 50",        672e6, 0.7e6, 50},
    {"rtx 40",        272e6, 0.5e6, 45},
    {"rtx 30",        360e6, 0.5e6, 45},
    {"rtx 20",        224e6, 0.4e6, 40},
};

const GpuSeedRow kDefaultSeed = {"", 200e6, 0.5e6, 40};

// Config first, then built-ins, then the default. Longest match wins within a
// tier, so "4060 laptop" beats "rtx 40".
GpuSeedRow resolve_seed(const std::string& gpu_name,
                     const std::vector<rf::GpuSeed>& configured) {
    const std::string name = lower(gpu_name);

    const rf::GpuSeed* best_cfg = nullptr;
    for (const auto& c : configured) {
        if (c.match.empty() || name.find(lower(c.match)) == std::string::npos) continue;
        if (!best_cfg || c.match.size() > best_cfg->match.size()) best_cfg = &c;
    }
    if (best_cfg)
        return GpuSeedRow{best_cfg->match.c_str(), best_cfg->mem_bw, best_cfg->load_bw,
                       best_cfg->prefill_ratio};

    const GpuSeedRow* best = nullptr;
    for (const auto& b : kGpuSeeds) {
        if (name.find(b.match) == std::string::npos) continue;
        if (!best || std::strlen(b.match) > std::strlen(best->match)) best = &b;
    }
    if (best) return *best;

    // Unmatched hardware gets a generic seed and the operator is told, once.
    // Guessing a node's speed in silence is how a scheduler ends up ranking a
    // weak machine level with a strong one and then explaining it confidently.
    static std::set<std::string> warned;
    static std::mutex warned_mu;
    {
        std::lock_guard<std::mutex> lock(warned_mu);
        if (warned.insert(name).second)
            RF_WARN("no hardware seed for '%s'; using a generic one. Its decode "
                    "and load estimates are guesses until Phase 2 learns them - "
                    "add an entry under cost.gpu_seeds to fix that.",
                    gpu_name.c_str());
    }
    return kDefaultSeed;
}

// KV cache per token, scaled by weight size. Calibrated against this project's
// own measurement rather than guessed: qwen2.5:7b-instruct-q4_K_M is 4.683 GB
// on disk and reports 4.748 GB resident, with free VRAM dropping by 4.858 GB
// across the load — so weights plus KV plus CUDA context came to about 3.7%
// over the on-disk size at Ollama's default context, not the ~20% the first
// guess implied.
//
// Erring high is still the right direction (an over-estimate loses a candidate,
// an under-estimate admits a node that cannot serve the request), but the first
// value was high enough to make a 6.8 GB model un-admissible on an 8 GB card,
// which is a machine that can obviously run it.
constexpr double kKvBytesPerTokenPerGiB = 8.0 * 1024.0;

// Used when the engine reports no size for a model at all. Deliberately large:
// under-estimating a footprint admits a node that cannot serve the request,
// which is a routing failure, while over-estimating only loses a candidate.
constexpr uint64_t kUnknownModelBytes = 6ULL * 1024 * 1024 * 1024;

// Seeded output-length prior. The sigma is intentionally near the mean: we know
// almost nothing before the request, and §6.2's noise check depends on that
// being stated honestly rather than flattered.
constexpr uint32_t kSeedOutputTokens = 256;
constexpr float kSeedOutputSigma = 200.f;

}  // namespace

SeedRates seed_rates(const NodeState& node, uint64_t footprint,
                     const ScoringConfig& scoring) {
    const GpuSeedRow seed = resolve_seed(node.gpu_name, scoring.gpu_seeds);
    const double weights = std::max(1.0, static_cast<double>(footprint));
    SeedRates out;
    // Decode is memory-bound: bandwidth over the bytes read per token.
    out.decode_tokens_per_ms = seed.mem_bw / weights;
    // Prefill is compute-bound, and runs a per-class multiple of decode.
    out.prefill_tokens_per_ms = out.decode_tokens_per_ms * seed.prefill_ratio;
    out.load_bytes_per_ms = seed.load_bw;
    out.matched = seed.match;
    return out;
}

uint64_t seed_footprint_bytes(const std::string& model, const NodeState& node,
                              uint32_t num_ctx, const ScoringConfig& scoring) {
    // Best evidence first: if the engine already holds it, that is the measured
    // answer and beats any estimate.
    const uint64_t resident = node.resident_bytes(model);
    if (resident > 0) return resident;

    uint64_t weights = node.disk_bytes(model);
    double margin = scoring.footprint_margin;
    if (weights == 0) {
        weights = kUnknownModelBytes;
        margin = 1.0;  // already a pessimistic guess; do not inflate twice
    }
    const double weights_gib = static_cast<double>(weights) / kGiB;
    const double ctx = num_ctx > 0 ? num_ctx : 4096;
    const double kv = ctx * kKvBytesPerTokenPerGiB * weights_gib;
    return static_cast<uint64_t>(static_cast<double>(weights) * margin + kv);
}

uint32_t seed_output_tokens() { return kSeedOutputTokens; }
float seed_output_sigma() { return kSeedOutputSigma; }

}  // namespace rf
