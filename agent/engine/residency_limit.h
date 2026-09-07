// How many models will this engine actually keep resident? (D38)
//
// `NodeState::engine_slots` says how many requests an engine accepts at once
// and nothing about how many models it holds, so the router priced every
// preload as free residency and Ollama -- with OLLAMA_MAX_LOADED_MODELS=1 --
// silently turned each one into a swap. Measured, placement moved cold starts
// around rather than removing them.
//
// Ollama does not expose the setting over its API, so it cannot be read. It can
// be watched for, and watching it needs care, because the first version of this
// looked for the wrong thing. It expected one poll to show a model gained and
// another lost together. What the engine actually does, measured at 2 Hz:
//
//     [qwen] -> [] -> [tinyllama]        1 to 5 seconds of nothing resident
//
// It drops first and loads second, so the resident set is empty in between and
// a same-poll comparison never fires. Hence the "last non-empty set" below.
//
// The evidence is three independent observations, because two of them have
// innocent explanations on their own:
//
//   1. A model left the resident set *before its own expires_at*. A model that
//      simply timed out proves nothing about capacity; one the engine dropped
//      early was dropped to make room. Measured: qwen went 4m51s before its
//      five-minute expiry, the moment tinyllama was asked for.
//   2. A different model arrived soon after, so the room was wanted for a load
//      rather than freed for some other reason.
//   3. There were already free bytes for the arrival, so it was not ordinary
//      VRAM pressure. That is the test that separates a count limit from a
//      memory limit, and it is the one a simulated node always fails -- which
//      is why the simulator can never invent a ceiling it does not have.
//
// Deliberately conservative in one direction. Declaring a limit that is not
// there makes the placement manager decline preloads it could have made, and
// makes admission price an eviction that would not have happened. Failing to
// declare one leaves everything exactly as it was. So the evidence has to
// arrive twice.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "common/types.h"

namespace rf {

class ResidencyLimit {
public:
    // One engine poll. `now_ms` is passed rather than read so a test can drive
    // the clock. Returns the demonstrated ceiling, or 0 for "unknown", which is
    // what the router must go on treating as "no information".
    uint32_t observe(const std::vector<ResidentModel>& resident, bool residency_known,
                     uint64_t vram_free_bytes, int64_t now_ms) {
        if (!residency_known) return limit_;

        const uint32_t n = static_cast<uint32_t>(resident.size());
        if (n > high_water_) {
            // The set grew past the ceiling we thought we had seen, so whatever
            // we concluded was about a smaller engine than this one.
            high_water_ = n;
            swaps_ = 0;
            limit_ = 0;
        }
        // An empty set is the middle of a swap as often as it is an idle
        // engine, and it can be compared with neither neighbour. Skipping it
        // without overwriting `prev_` is what lets the two ends be compared.
        if (n == 0) return limit_;

        if (have_prev_ && now_ms - prev_ms_ <= kMaxSwapGapMs) {
            uint64_t gained_bytes = 0;
            bool gained = false;
            for (const auto& m : resident)
                if (!held(m.name)) {
                    gained = true;
                    gained_bytes += m.vram_bytes;
                }

            bool dropped_early = false;
            for (const Held& was : prev_) {
                bool still = false;
                for (const auto& m : resident)
                    if (m.name == was.name) { still = true; break; }
                // Expiry unknown (0) is not evidence: an engine that does not
                // say when it will drop a model cannot tell us it dropped one
                // sooner than it meant to.
                if (!still && was.expires_at_ms > prev_ms_) dropped_early = true;
            }

            if (gained && dropped_early && gained_bytes > 0 &&
                prev_free_ >= gained_bytes) {
                if (++swaps_ >= kSwapsToBelieve) limit_ = high_water_;
            }
        }

        prev_.clear();
        prev_.reserve(resident.size());
        for (const auto& m : resident) prev_.push_back({m.name, m.expires_at_ms});
        prev_free_ = vram_free_bytes;
        prev_ms_ = now_ms;
        have_prev_ = true;
        return limit_;
    }

    uint32_t value() const { return limit_; }

private:
    struct Held {
        std::string name;
        int64_t expires_at_ms;
    };

    bool held(const std::string& name) const {
        for (const Held& h : prev_)
            if (h.name == name) return true;
        return false;
    }

    // Long enough for a large model to load, far short of any keep_alive worth
    // the name. Beyond it the two observations are not one event and comparing
    // them would be comparing an idle engine with a busy one.
    static const int64_t kMaxSwapGapMs = 30000;
    // Two, not one. A single coincidence is cheap to wait out and expensive to
    // act on.
    static const uint32_t kSwapsToBelieve = 2;

    std::vector<Held> prev_;  // the last non-empty resident set
    uint64_t prev_free_ = 0;
    int64_t prev_ms_ = 0;
    bool have_prev_ = false;
    uint32_t high_water_ = 0;
    uint32_t swaps_ = 0;
    uint32_t limit_ = 0;
};

}  // namespace rf
