// RouteFlow — shared domain types (ARCHITECTURE §5).
//
// These are the contract between agent, router, cost model, policy, trace and
// UI. They live in /common precisely because more than one component depends on
// each of them; putting Estimate in the cost model, or NodeState in the agent,
// would make the dependency arrows point the wrong way.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "json.h"

namespace rf {

// --- engines ----------------------------------------------------------------

enum class EngineKind { None, Ollama, LMStudio, Simulated };

const char* to_string(EngineKind k);
EngineKind engine_kind_from_string(const std::string& s);

// --- node state -------------------------------------------------------------

struct ResidentModel {
    std::string name;
    uint64_t vram_bytes = 0;
    int64_t last_used_ms = 0;  // epoch ms; 0 = never observed by us
};

struct NodeState {
    std::string id;
    std::string gpu_name;
    std::string telemetry_backend = "null";  // "nvml" | "tegra" | "null" | "sim"
    bool telemetry_ok = false;               // false => util/power/temp absent

    uint64_t vram_total_bytes = 0;
    uint64_t vram_free_bytes = 0;
    float gpu_util = 0.f;  // 0..1, smoothed
    float power_watts = 0.f;
    float power_cap_watts = 0.f;  // 0 = unknown
    float temperature_c = 0.f;

    EngineKind engine = EngineKind::None;
    bool engine_healthy = false;
    uint32_t engine_slots = 1;  // parallel requests the engine accepts
    // Some engines do not expose which models are in VRAM. Treating "unknown"
    // as "cold" would silently invent the one number this project exists to
    // measure, so the flag travels with the state and omits T_load instead.
    bool residency_known = true;

    std::vector<std::string> models_on_disk;
    std::vector<ResidentModel> models_resident;
    // Model sizes on disk, when the engine reports them. Seeds footprint (D2).
    std::vector<std::pair<std::string, uint64_t>> model_disk_bytes;

    uint32_t inflight_reported = 0;  // engine's own view; advisory only
    int64_t sampled_at_ms = 0;

    bool is_resident(const std::string& model) const;
    uint64_t resident_bytes(const std::string& model) const;
    bool has_on_disk(const std::string& model) const;
    uint64_t disk_bytes(const std::string& model) const;  // 0 if unknown

    Json to_json() const;
    static bool from_json(const Json& j, NodeState& out);
};

// --- request ----------------------------------------------------------------

struct RequestFeatures {
    std::string model;
    uint32_t prompt_tokens = 0;            // estimated (D15)
    uint32_t predicted_output_tokens = 0;  // from the cost model
    float predicted_output_sigma = 0.f;    // 1-sigma, tokens
    uint32_t num_ctx = 0;                  // 0 = engine default
    bool stream = false;
    std::string role_hint;  // optional; never required
};

// --- estimation -------------------------------------------------------------

enum class Confidence { Seeded, Learning, Converged };
const char* to_string(Confidence c);

// Terms that can be omitted for lack of signal. Never silently zero
// (ARCHITECTURE §10): an omitted term is flagged all the way to the UI.
enum TermFlag : uint32_t {
    kTermThermal = 1u << 0,
    kTermQueue = 1u << 1,
    kTermLoad = 1u << 2,
    kTermPrefill = 1u << 3,
    kTermDecode = 1u << 4,
};
std::vector<std::string> term_flag_names(uint32_t mask);

struct Estimate {
    double t_queue_ms = 0;
    double t_load_ms = 0;
    double t_prefill_ms = 0;
    double t_decode_ms = 0;
    double t_evict_ms = 0;  // externality of evicting a warm model (§6.2)

    double sigma_ms = 0;  // 1-sigma on the total
    Confidence conf = Confidence::Seeded;
    uint32_t samples = 0;
    uint32_t omitted_terms = 0;

    double total_ms() const {
        return t_queue_ms + t_load_ms + t_prefill_ms + t_decode_ms + t_evict_ms;
    }
    // Name of the largest term. Feeds Decision::decided_by.
    const char* dominant_term() const;
};

struct OutputPrediction {
    uint32_t tokens = 0;
    float sigma = 0.f;
    Confidence conf = Confidence::Seeded;
};

// --- decision ---------------------------------------------------------------

enum class AdmitReason {
    Ok,
    EngineDown,
    ModelMissing,
    InsufficientVram,
    Excluded,
    NodeStale,
};
const char* to_string(AdmitReason r);

struct Candidate {
    std::string node_id;
    bool admitted = false;
    AdmitReason reason = AdmitReason::Ok;
    Estimate est;

    // Node state at decision time. Recorded for losers as well as the winner:
    // counterfactual analysis is impossible without it (D10).
    uint64_t vram_free_bytes = 0;
    float gpu_util = 0.f;
    bool telemetry_ok = false;
    bool was_resident = false;
    uint32_t inflight = 0;
    std::vector<std::string> would_evict;

    Json to_json() const;
};

struct Decision {
    std::string winner_node_id;  // empty => no admissible candidate
    std::vector<Candidate> candidates;
    std::string decided_by;  // dominant term, "within_noise", or "single_candidate"
    double margin_ms = 0;    // winner vs runner-up; 0 with one candidate
    bool has_runner_up = false;
    std::string policy_name;

    const Candidate* winner() const;
};

// --- ledger view ------------------------------------------------------------

// What the router knows synchronously about a node, independent of telemetry
// (ARCHITECTURE §6.3). The cost model reads this through a view so that it does
// not depend on the ledger's implementation.
struct PendingLoad {
    std::string model;
    int64_t started_ms = 0;
    double eta_ms = 0;
};

class LedgerView {
public:
    virtual ~LedgerView() = default;
    virtual uint32_t inflight(const std::string& node_id) const = 0;
    virtual uint32_t concurrent_decoders(const std::string& node_id) const = 0;
    virtual uint64_t reserved_vram_bytes(const std::string& node_id) const = 0;
    // Non-null when this router already started loading `model` on `node_id`.
    virtual const PendingLoad* pending(const std::string& node_id,
                                       const std::string& model) const = 0;
    // Models this router is currently generating with, so they are not evictable.
    virtual bool model_busy(const std::string& node_id,
                            const std::string& model) const = 0;
};

// A LedgerView that reports an idle cluster. Used by tests and by the bench
// replayer, where no dispatch has happened.
class EmptyLedger : public LedgerView {
public:
    uint32_t inflight(const std::string&) const override { return 0; }
    uint32_t concurrent_decoders(const std::string&) const override { return 0; }
    uint64_t reserved_vram_bytes(const std::string&) const override { return 0; }
    const PendingLoad* pending(const std::string&, const std::string&) const override {
        return nullptr;
    }
    bool model_busy(const std::string&, const std::string&) const override {
        return false;
    }
};

// --- trace ------------------------------------------------------------------

enum class Outcome {
    Ok,
    NoCandidate,
    DispatchFailed,
    StreamFailed,
    ClientAbort,
    Timeout,
};
const char* to_string(Outcome o);

// One record per dispatch attempt. Mirrors docs/TRACE-SCHEMA.md v1 exactly.
// A value that is unknown is null in the JSON, which is why the optional
// fields carry an explicit has_* flag rather than a sentinel: load_ms == 0
// means "the model was resident", not "we failed to measure it".
struct TraceRecord {
    static constexpr int kSchemaVersion = 1;

    std::string job_id;
    std::string retry_of;  // empty => null

    int64_t ts_received_ms = 0;
    int64_t ts_dispatched_ms = 0;
    int64_t ts_first_token_ms = 0;  // 0 => null
    int64_t ts_done_ms = 0;

    std::string model;
    std::string role_hint;
    uint32_t num_ctx = 0;
    bool stream = false;

    std::string policy;
    std::string cost_model;
    std::string node_id;
    bool was_resident = false;

    uint32_t prompt_tokens_est = 0;
    bool has_prompt_tokens_actual = false;
    uint32_t prompt_tokens_actual = 0;
    bool has_output_tokens = false;
    uint32_t output_tokens = 0;

    uint32_t predicted_output_tokens = 0;
    float predicted_output_sigma = 0.f;
    double predicted_total_ms = 0;
    double predicted_sigma_ms = 0;

    double queue_wait_ms = 0;
    bool has_load_ms = false;  // false => we could not measure it (null in JSON)
    double load_ms = 0;
    bool has_ttft = false;
    double ttft_ms = 0;
    double total_ms = 0;

    uint32_t inflight_at_dispatch = 0;
    uint32_t concurrent_decoders_at_dispatch = 0;
    bool telemetry_ok_at_dispatch = false;
    float gpu_util_at_dispatch = 0.f;
    uint64_t vram_free_at_dispatch = 0;
    std::string telemetry_backend;

    std::vector<std::string> evicted;
    std::string decided_by;
    bool has_margin = false;
    double margin_ms = 0;

    std::vector<Candidate> candidates;

    Outcome outcome = Outcome::Ok;
    std::string error;  // empty => null

    Json to_json() const;
    static bool from_json(const Json& j, TraceRecord& out, std::string* err);
};

}  // namespace rf
