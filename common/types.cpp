#include "types.h"

#include <algorithm>

#include "util.h"

namespace rf {
namespace {

// A null Json for fields documented as "unknown" rather than zero.
Json maybe_num(bool present, double v) { return present ? Json(v) : Json(); }
Json maybe_str(const std::string& s) { return s.empty() ? Json() : Json(s); }

Json string_array(const std::vector<std::string>& v) {
    Json a = Json::array();
    for (const auto& s : v) a.push_back(Json(s));
    return a;
}

std::vector<std::string> to_string_vector(const Json& j) {
    std::vector<std::string> out;
    for (size_t i = 0; i < j.size(); ++i) out.push_back(j.at(i).as_str());
    return out;
}

}  // namespace

// --- enums ------------------------------------------------------------------

const char* to_string(EngineKind k) {
    switch (k) {
        case EngineKind::Ollama:    return "ollama";
        case EngineKind::LMStudio:  return "lmstudio";
        case EngineKind::Simulated: return "simulated";
        case EngineKind::None:      return "none";
    }
    return "none";
}

EngineKind engine_kind_from_string(const std::string& s) {
    const std::string n = lower(trim(s));
    if (n == "ollama") return EngineKind::Ollama;
    if (n == "lmstudio" || n == "lm-studio" || n == "lm_studio") return EngineKind::LMStudio;
    if (n == "simulated" || n == "sim") return EngineKind::Simulated;
    return EngineKind::None;
}

const char* to_string(Confidence c) {
    switch (c) {
        case Confidence::Seeded:    return "seeded";
        case Confidence::Learning:  return "learning";
        case Confidence::Converged: return "converged";
    }
    return "seeded";
}

const char* to_string(AdmitReason r) {
    switch (r) {
        case AdmitReason::Ok:               return "ok";
        case AdmitReason::EngineDown:       return "engine_down";
        case AdmitReason::ModelMissing:     return "model_missing";
        case AdmitReason::InsufficientVram: return "insufficient_vram";
        case AdmitReason::Excluded:         return "excluded";
        case AdmitReason::NodeStale:        return "node_stale";
    }
    return "ok";
}

const char* to_string(Outcome o) {
    switch (o) {
        case Outcome::Ok:             return "ok";
        case Outcome::NoCandidate:    return "no_candidate";
        case Outcome::DispatchFailed: return "dispatch_failed";
        case Outcome::StreamFailed:   return "stream_failed";
        case Outcome::ClientAbort:    return "client_abort";
        case Outcome::Timeout:        return "timeout";
    }
    return "ok";
}

std::vector<std::string> term_flag_names(uint32_t mask) {
    std::vector<std::string> out;
    if (mask & kTermThermal) out.push_back("thermal");
    if (mask & kTermQueue)   out.push_back("t_queue");
    if (mask & kTermLoad)    out.push_back("t_load");
    if (mask & kTermPrefill) out.push_back("t_prefill");
    if (mask & kTermDecode)  out.push_back("t_decode");
    return out;
}

// --- NodeState --------------------------------------------------------------

bool NodeState::is_resident(const std::string& model) const {
    for (const auto& m : models_resident)
        if (m.name == model) return true;
    return false;
}

uint64_t NodeState::resident_bytes(const std::string& model) const {
    for (const auto& m : models_resident)
        if (m.name == model) return m.vram_bytes;
    return 0;
}

bool NodeState::has_on_disk(const std::string& model) const {
    return std::find(models_on_disk.begin(), models_on_disk.end(), model) !=
           models_on_disk.end();
}

uint64_t NodeState::disk_bytes(const std::string& model) const {
    for (const auto& kv : model_disk_bytes)
        if (kv.first == model) return kv.second;
    return 0;
}

Json NodeState::to_json() const {
    Json j = Json::object();
    j["id"] = Json(id);
    j["gpu_name"] = Json(gpu_name);
    j["telemetry_backend"] = Json(telemetry_backend);
    j["telemetry_ok"] = Json(telemetry_ok);
    j["vram_total_bytes"] = Json(vram_total_bytes);
    j["vram_free_bytes"] = Json(vram_free_bytes);
    // Telemetry-derived scalars are null, not zero, when there is no backend.
    j["gpu_util"] = maybe_num(telemetry_ok, gpu_util);
    j["power_watts"] = maybe_num(telemetry_ok, power_watts);
    j["power_cap_watts"] = maybe_num(telemetry_ok && power_cap_watts > 0, power_cap_watts);
    j["temperature_c"] = maybe_num(telemetry_ok, temperature_c);
    j["engine"] = Json(to_string(engine));
    j["engine_healthy"] = Json(engine_healthy);
    j["engine_slots"] = Json(engine_slots);
    // Null rather than 0 while undemonstrated: "we have not seen a ceiling" is
    // not "the ceiling is zero" (§10).
    j["models_resident_limit"] =
        models_resident_limit > 0 ? Json(models_resident_limit) : Json();
    j["residency_known"] = Json(residency_known);
    j["models_on_disk"] = string_array(models_on_disk);

    Json res = Json::array();
    for (const auto& m : models_resident) {
        Json e = Json::object();
        e["name"] = Json(m.name);
        e["vram_bytes"] = Json(m.vram_bytes);
        e["last_used_ms"] = Json(m.last_used_ms);
        e["expires_at_ms"] = m.expires_at_ms > 0 ? Json(m.expires_at_ms) : Json();
        res.push_back(std::move(e));
    }
    j["models_resident"] = std::move(res);

    Json sizes = Json::object();
    for (const auto& kv : model_disk_bytes) sizes[kv.first] = Json(kv.second);
    j["model_disk_bytes"] = std::move(sizes);

    j["inflight_reported"] = Json(inflight_reported);
    j["sampled_at_ms"] = Json(sampled_at_ms);
    return j;
}

bool NodeState::from_json(const Json& j, NodeState& out) {
    if (!j.is_object()) return false;
    out = NodeState();
    out.id = j["id"].as_str();
    out.gpu_name = j["gpu_name"].as_str();
    out.telemetry_backend = j["telemetry_backend"].as_str("null");
    out.telemetry_ok = j["telemetry_ok"].as_bool();
    out.vram_total_bytes = j["vram_total_bytes"].as_u64();
    out.vram_free_bytes = j["vram_free_bytes"].as_u64();
    out.gpu_util = static_cast<float>(j["gpu_util"].as_num(0));
    out.power_watts = static_cast<float>(j["power_watts"].as_num(0));
    out.power_cap_watts = static_cast<float>(j["power_cap_watts"].as_num(0));
    out.temperature_c = static_cast<float>(j["temperature_c"].as_num(0));
    out.engine = engine_kind_from_string(j["engine"].as_str());
    out.engine_healthy = j["engine_healthy"].as_bool();
    out.engine_slots = std::max(1u, j["engine_slots"].as_u32(1));
    // Absent from an older agent, which is the same as undemonstrated.
    out.models_resident_limit = j["models_resident_limit"].as_u32(0);
    // Absent in a state from an older agent: assume known, which is what every
    // engine except an old LM Studio actually reports.
    out.residency_known = j.has("residency_known") ? j["residency_known"].as_bool() : true;
    out.models_on_disk = to_string_vector(j["models_on_disk"]);

    const Json& res = j["models_resident"];
    for (size_t i = 0; i < res.size(); ++i) {
        ResidentModel m;
        m.name = res.at(i)["name"].as_str();
        m.vram_bytes = res.at(i)["vram_bytes"].as_u64();
        m.last_used_ms = res.at(i)["last_used_ms"].as_i64();
        m.expires_at_ms = res.at(i)["expires_at_ms"].as_i64();
        if (!m.name.empty()) out.models_resident.push_back(std::move(m));
    }
    for (const auto& kv : j["model_disk_bytes"].fields())
        out.model_disk_bytes.emplace_back(kv.first, kv.second.as_u64());

    out.inflight_reported = j["inflight_reported"].as_u32();
    out.sampled_at_ms = j["sampled_at_ms"].as_i64();
    return !out.id.empty();
}

// --- Estimate ---------------------------------------------------------------

const char* Estimate::dominant_term() const {
    const double terms[5] = {t_queue_ms, t_load_ms, t_prefill_ms, t_decode_ms, t_evict_ms};
    const char* names[5] = {"t_queue", "t_load", "t_prefill", "t_decode", "t_evict"};
    int best = 0;
    for (int i = 1; i < 5; ++i)
        if (terms[i] > terms[best]) best = i;
    return terms[best] > 0 ? names[best] : "none";
}

// --- Candidate / Decision ---------------------------------------------------

Json Candidate::to_json() const {
    Json j = Json::object();
    j["node_id"] = Json(node_id);
    j["admitted"] = Json(admitted);
    j["reason"] = Json(to_string(reason));
    if (admitted) {
        j["predicted_total_ms"] = Json(est.total_ms());
        j["t_queue"] = Json(est.t_queue_ms);
        j["t_load"] = Json(est.t_load_ms);
        j["t_prefill"] = Json(est.t_prefill_ms);
        j["t_decode"] = Json(est.t_decode_ms);
        j["t_evict"] = Json(est.t_evict_ms);
        j["sigma_ms"] = Json(est.sigma_ms);
        j["conf"] = Json(to_string(est.conf));
        j["samples"] = Json(est.samples);
    } else {
        // Not admitted: no prediction was made. Null, not zero.
        j["predicted_total_ms"] = Json();
        j["t_queue"] = Json();
        j["t_load"] = Json();
        j["t_prefill"] = Json();
        j["t_decode"] = Json();
        j["t_evict"] = Json();
        j["sigma_ms"] = Json();
        j["conf"] = Json(to_string(est.conf));
    }
    j["omitted"] = string_array(term_flag_names(est.omitted_terms));
    j["vram_free"] = Json(vram_free_bytes);
    j["gpu_util"] = maybe_num(telemetry_ok, gpu_util);
    j["was_resident"] = Json(was_resident);
    j["inflight"] = Json(inflight);
    if (!would_evict.empty()) j["would_evict"] = string_array(would_evict);
    return j;
}

const Candidate* Decision::winner() const {
    if (winner_node_id.empty()) return nullptr;
    for (const auto& c : candidates)
        if (c.node_id == winner_node_id) return &c;
    return nullptr;
}

// --- TraceRecord ------------------------------------------------------------

Json TraceRecord::to_json() const {
    Json j = Json::object();
    j["v"] = Json(kSchemaVersion);
    j["job_id"] = Json(job_id);
    j["retry_of"] = maybe_str(retry_of);

    j["ts_received"] = Json(iso8601(ts_received_ms));
    j["ts_dispatched"] = ts_dispatched_ms ? Json(iso8601(ts_dispatched_ms)) : Json();
    j["ts_first_token"] = ts_first_token_ms ? Json(iso8601(ts_first_token_ms)) : Json();
    j["ts_done"] = Json(iso8601(ts_done_ms));

    j["model"] = Json(model);
    j["role_hint"] = maybe_str(role_hint);
    j["num_ctx"] = Json(num_ctx);
    j["stream"] = Json(stream);

    j["policy"] = Json(policy);
    j["cost_model"] = Json(cost_model);
    j["node_id"] = Json(node_id);
    j["was_resident"] = Json(was_resident);
    j["footprint_bytes"] = maybe_num(footprint_bytes > 0,
                                     static_cast<double>(footprint_bytes));

    j["prompt_tokens_est"] = Json(prompt_tokens_est);
    j["prompt_tokens_actual"] = maybe_num(has_prompt_tokens_actual, prompt_tokens_actual);
    j["output_tokens"] = maybe_num(has_output_tokens, output_tokens);

    j["predicted_output_tokens"] = Json(predicted_output_tokens);
    j["predicted_output_sigma"] = Json(predicted_output_sigma);
    j["predicted_total_ms"] = Json(predicted_total_ms);
    j["predicted_sigma_ms"] = Json(predicted_sigma_ms);

    j["queue_wait_ms"] = Json(queue_wait_ms);
    j["load_ms"] = maybe_num(has_load_ms, load_ms);
    j["ttft_ms"] = maybe_num(has_ttft, ttft_ms);
    j["total_ms"] = Json(total_ms);

    j["inflight_at_dispatch"] = Json(inflight_at_dispatch);
    j["concurrent_decoders_at_dispatch"] = Json(concurrent_decoders_at_dispatch);
    j["concurrent_decoders_at_first_token"] =
        concurrent_decoders_at_first_token > 0
            ? Json(concurrent_decoders_at_first_token)
            : Json();
    j["gpu_util_at_dispatch"] = maybe_num(telemetry_ok_at_dispatch, gpu_util_at_dispatch);
    j["vram_free_at_dispatch"] =
        maybe_num(telemetry_ok_at_dispatch, static_cast<double>(vram_free_at_dispatch));
    j["telemetry_backend"] = Json(telemetry_backend);

    j["evicted"] = string_array(evicted);
    j["decided_by"] = Json(decided_by);
    j["margin_ms"] = maybe_num(has_margin, margin_ms);

    Json cands = Json::array();
    for (const auto& c : candidates) cands.push_back(c.to_json());
    j["candidates"] = std::move(cands);

    j["outcome"] = Json(to_string(outcome));
    j["error"] = maybe_str(error);
    return j;
}

bool TraceRecord::from_json(const Json& j, TraceRecord& out, std::string* err) {
    if (!j.is_object()) {
        if (err) *err = "not an object";
        return false;
    }
    const int64_t v = j["v"].as_i64(0);
    if (v < 1) {
        if (err) *err = "missing or bad schema version";
        return false;
    }
    if (v > kSchemaVersion) {
        // Rule 3: unknown fields are ignored, so a newer record still parses;
        // the reader just may not see everything in it.
        if (err) *err = "record from a newer schema version (read partially)";
    }

    out = TraceRecord();
    out.job_id = j["job_id"].as_str();
    out.ts_received_ms = parse_iso8601(j["ts_received"].as_str());
    out.ts_dispatched_ms = parse_iso8601(j["ts_dispatched"].as_str());
    out.ts_first_token_ms = parse_iso8601(j["ts_first_token"].as_str());
    out.ts_done_ms = parse_iso8601(j["ts_done"].as_str());
    out.retry_of = j["retry_of"].as_str();
    out.model = j["model"].as_str();
    out.role_hint = j["role_hint"].as_str();
    out.num_ctx = j["num_ctx"].as_u32();
    out.stream = j["stream"].as_bool();
    out.policy = j["policy"].as_str();
    out.cost_model = j["cost_model"].as_str();
    out.node_id = j["node_id"].as_str();
    out.was_resident = j["was_resident"].as_bool();
    out.footprint_bytes = j["footprint_bytes"].as_u64(0);

    out.prompt_tokens_est = j["prompt_tokens_est"].as_u32();
    out.has_prompt_tokens_actual = j["prompt_tokens_actual"].is_num();
    out.prompt_tokens_actual = j["prompt_tokens_actual"].as_u32();
    out.has_output_tokens = j["output_tokens"].is_num();
    out.output_tokens = j["output_tokens"].as_u32();

    out.predicted_output_tokens = j["predicted_output_tokens"].as_u32();
    out.predicted_output_sigma = static_cast<float>(j["predicted_output_sigma"].as_num());
    out.predicted_total_ms = j["predicted_total_ms"].as_num();
    out.predicted_sigma_ms = j["predicted_sigma_ms"].as_num();

    out.queue_wait_ms = j["queue_wait_ms"].as_num();
    out.has_load_ms = j["load_ms"].is_num();
    out.load_ms = j["load_ms"].as_num();
    out.has_ttft = j["ttft_ms"].is_num();
    out.ttft_ms = j["ttft_ms"].as_num();
    out.total_ms = j["total_ms"].as_num();

    out.inflight_at_dispatch = j["inflight_at_dispatch"].as_u32();
    out.concurrent_decoders_at_dispatch = j["concurrent_decoders_at_dispatch"].as_u32();
    out.concurrent_decoders_at_first_token =
        j["concurrent_decoders_at_first_token"].as_u32();
    out.telemetry_ok_at_dispatch = j["gpu_util_at_dispatch"].is_num();
    out.gpu_util_at_dispatch = static_cast<float>(j["gpu_util_at_dispatch"].as_num());
    out.vram_free_at_dispatch = j["vram_free_at_dispatch"].as_u64();
    out.telemetry_backend = j["telemetry_backend"].as_str();

    out.evicted = to_string_vector(j["evicted"]);
    out.decided_by = j["decided_by"].as_str();
    out.has_margin = j["margin_ms"].is_num();
    out.margin_ms = j["margin_ms"].as_num();

    const Json& cands = j["candidates"];
    for (size_t i = 0; i < cands.size(); ++i) {
        const Json& cj = cands.at(i);
        Candidate c;
        c.node_id = cj["node_id"].as_str();
        c.admitted = cj["admitted"].as_bool();
        const std::string reason = cj["reason"].as_str("ok");
        if (reason == "engine_down") c.reason = AdmitReason::EngineDown;
        else if (reason == "model_missing") c.reason = AdmitReason::ModelMissing;
        else if (reason == "insufficient_vram") c.reason = AdmitReason::InsufficientVram;
        else if (reason == "excluded") c.reason = AdmitReason::Excluded;
        else if (reason == "node_stale") c.reason = AdmitReason::NodeStale;
        else c.reason = AdmitReason::Ok;
        c.est.t_queue_ms = cj["t_queue"].as_num();
        c.est.t_load_ms = cj["t_load"].as_num();
        c.est.t_prefill_ms = cj["t_prefill"].as_num();
        c.est.t_decode_ms = cj["t_decode"].as_num();
        c.est.t_evict_ms = cj["t_evict"].as_num();
        c.est.sigma_ms = cj["sigma_ms"].as_num();
        c.est.samples = cj["samples"].as_u32();
        c.vram_free_bytes = cj["vram_free"].as_u64();
        c.telemetry_ok = cj["gpu_util"].is_num();
        c.gpu_util = static_cast<float>(cj["gpu_util"].as_num());
        c.was_resident = cj["was_resident"].as_bool();
        c.inflight = cj["inflight"].as_u32();
        c.would_evict = to_string_vector(cj["would_evict"]);
        out.candidates.push_back(std::move(c));
    }

    const std::string outcome = j["outcome"].as_str("ok");
    if (outcome == "no_candidate") out.outcome = Outcome::NoCandidate;
    else if (outcome == "dispatch_failed") out.outcome = Outcome::DispatchFailed;
    else if (outcome == "stream_failed") out.outcome = Outcome::StreamFailed;
    else if (outcome == "client_abort") out.outcome = Outcome::ClientAbort;
    else if (outcome == "timeout") out.outcome = Outcome::Timeout;
    else out.outcome = Outcome::Ok;
    out.error = j["error"].as_str();

    return !out.job_id.empty();
}

}  // namespace rf
