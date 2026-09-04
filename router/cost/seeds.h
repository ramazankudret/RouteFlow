// RouteFlow — hardware seed rates (ARCHITECTURE §8).
//
// The rates a cost model starts from before it has observed anything. Shared by
// StaticCostModel, which never leaves them, and LearnedCostModel, which uses
// them as the cold-start prior for each quantity independently — a node can
// have a learned decode rate and a seeded load bandwidth at the same time, and
// usually does, because cold starts are rarer than generations.
#pragma once

#include <cstdint>
#include <string>

#include "common/types.h"
#include "router/core/interfaces.h"

namespace rf {

struct SeedRates {
    double decode_tokens_per_ms = 0;   // at concurrency 1
    double prefill_tokens_per_ms = 0;
    double load_bytes_per_ms = 0;
    const char* matched = "";          // which table entry answered, "" if none
};

// `footprint` matters because decode is memory-bound: the rate is device
// bandwidth divided by the bytes that must be read per token.
SeedRates seed_rates(const NodeState& node, uint64_t footprint,
                     const ScoringConfig& scoring);

// Footprint estimate used before any load of this model on this node has been
// observed. Exposed because both cost models need the same answer, and because
// admission depends on it (D2).
uint64_t seed_footprint_bytes(const std::string& model, const NodeState& node,
                              uint32_t num_ctx, const ScoringConfig& scoring);

// Seeded output-length prior, for a request whose caller gave no cap.
uint32_t seed_output_tokens();
float seed_output_sigma();

}  // namespace rf
