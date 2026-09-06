#include "router/core/dispatch.h"

#include <algorithm>

#include "common/json.h"
#include "common/util.h"
#include "router/core/ingest.h"

namespace rf {
namespace {

constexpr size_t kMaxTailBytes = 64 * 1024;  // enough to hold the final frame

// What one proxy attempt measured.
struct AttemptResult {
    bool ok = false;
    bool sent_to_client = false;  // once true, retry is off the table (§6.4)
    Outcome outcome = Outcome::DispatchFailed;
    std::string error;

    int upstream_status = 0;
    int64_t ts_dispatched_ms = 0;
    int64_t ts_first_byte_ms = 0;
    int64_t ts_done_ms = 0;

    bool has_prompt_tokens = false;
    uint32_t prompt_tokens = 0;
    bool has_output_tokens = false;
    uint32_t output_tokens = 0;
    bool has_load_ms = false;
    double load_ms = 0;
};

// Engines report their counters in the LAST frame of a stream, or in the body
// of a buffered reply. Rather than parse the whole stream, keep a rolling tail
// and pull the numbers out of it at the end.
class TailBuffer {
public:
    void append(const char* data, size_t n) {
        tail_.append(data, n);
        if (tail_.size() > kMaxTailBytes)
            tail_.erase(0, tail_.size() - kMaxTailBytes);
    }
    const std::string& str() const { return tail_; }

private:
    std::string tail_;
};

// Pulls usage counters out of an Ollama object or an OpenAI chunk/response.
// Returns true if anything was found.
bool harvest_counters(const Json& j, AttemptResult& out) {
    bool found = false;

    // Ollama: prompt_eval_count / eval_count / load_duration (nanoseconds).
    if (j.has("eval_count")) {
        out.output_tokens = j["eval_count"].as_u32();
        out.has_output_tokens = true;
        found = true;
    }
    if (j.has("prompt_eval_count")) {
        out.prompt_tokens = j["prompt_eval_count"].as_u32();
        out.has_prompt_tokens = true;
        found = true;
    }
    if (j.has("load_duration")) {
        out.load_ms = j["load_duration"].as_num() / 1e6;
        out.has_load_ms = true;
        found = true;
    }

    // OpenAI: usage{prompt_tokens, completion_tokens}.
    const Json& usage = j["usage"];
    if (usage.is_object()) {
        if (usage.has("completion_tokens")) {
            out.output_tokens = usage["completion_tokens"].as_u32();
            out.has_output_tokens = true;
            found = true;
        }
        if (usage.has("prompt_tokens")) {
            out.prompt_tokens = usage["prompt_tokens"].as_u32();
            out.has_prompt_tokens = true;
            found = true;
        }
    }
    return found;
}

// Scans the tail backwards for the last parseable JSON object, whether the
// stream was NDJSON (Ollama) or SSE (OpenAI). Both engines put the counters in
// the final frame, so the last object that carries any is the one we want.
void harvest_from_tail(const std::string& tail, AttemptResult& out) {
    std::vector<std::string> frames;
    for (const auto& raw_line : split(tail, '\n')) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        if (starts_with(line, "data:")) line = trim(line.substr(5));
        if (line == "[DONE]") continue;
        if (line.empty() || line.front() != '{') continue;
        frames.push_back(std::move(line));
    }
    // A buffered (non-streamed) reply is one object and may not end in a
    // newline, so also try the whole tail.
    frames.push_back(trim(tail));

    for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
        Json j;
        if (!Json::parse(*it, j, nullptr)) continue;
        if (harvest_counters(j, out)) return;
    }
}

}  // namespace

Dispatcher::Dispatcher(RouterState& state, NodeRegistry& registry, TraceWriter& trace,
                       std::string node_token, int request_timeout_ms, bool request_usage)
    : state_(state),
      registry_(registry),
      trace_(trace),
      node_token_(std::move(node_token)),
      request_timeout_ms_(request_timeout_ms),
      request_usage_(request_usage) {}

void Dispatcher::handle(const http::Request& client_req, http::Responder& res) {
    const int64_t ts_received = now_ms();

    IngestResult in;
    if (!ingest(client_req, in, request_usage_)) {
        res.send_error(in.error_type == "not_found" ? 404 : 400, in.error_type,
                       in.error);
        return;
    }

    std::vector<std::string> tried;
    std::string previous_job_id;
    constexpr int kMaxAttempts = 2;  // one dispatch + one retry (§6.4, D9)

    for (int attempt_no = 0; attempt_no < kMaxAttempts; ++attempt_no) {
        const std::string job_id = ulid();

        // --- score and reserve, atomically (§6.3) ---------------------------
        Reservation r = tried.empty() ? state_.score_and_reserve(in.features)
                                      : state_.score_and_reserve_excluding(in.features,
                                                                           tried);

        // --- no candidate ---------------------------------------------------
        if (!r.ok) {
            TraceRecord rec;
            rec.job_id = job_id;
            rec.retry_of = previous_job_id;
            rec.ts_received_ms = ts_received;
            rec.ts_done_ms = now_ms();
            rec.model = in.features.model;
            rec.role_hint = in.features.role_hint;
            rec.num_ctx = in.features.num_ctx;
            rec.stream = in.features.stream;
            rec.policy = r.policy_name;
            rec.cost_model = r.cost_model_name;
            rec.prompt_tokens_est = in.features.prompt_tokens;
            rec.predicted_output_tokens = in.features.predicted_output_tokens;
            rec.predicted_output_sigma = in.features.predicted_output_sigma;
            rec.decided_by = r.decision.decided_by;
            rec.candidates = r.decision.candidates;
            rec.outcome = Outcome::NoCandidate;
            rec.total_ms = static_cast<double>(rec.ts_done_ms - ts_received);

            // Name the binding constraint per node: "no node available" with no
            // reason is the least useful error a scheduler can produce.
            std::string detail;
            for (const auto& c : r.decision.candidates) {
                if (!detail.empty()) detail += "; ";
                detail += c.node_id + ": " + to_string(c.reason);
            }
            if (detail.empty()) detail = "no nodes configured or none has reported yet";
            rec.error = detail;

            trace_.append(rec);
            state_.complete(NodeLedger::kInvalid, rec);
            res.send_error(503, "no_candidate",
                           "no node can serve model '" + in.features.model + "' (" +
                               detail + ")");
            return;
        }

        const NodeConfig* cfg = registry_.config_for(r.node_id);
        if (!cfg) {  // config reloaded between scoring and dispatch
            state_.complete(r.token, TraceRecord());
            tried.push_back(r.node_id);
            continue;
        }

        // --- proxy ----------------------------------------------------------
        AttemptResult a;
        TailBuffer tail;
        bool client_gone = false;

        http::ClientRequest up;
        up.host = cfg->dispatch_host();
        up.port = cfg->dispatch_port();
        up.method = "POST";
        up.path = in.upstream_path;
        // Byte for byte, except for the usage request ingest may have added
        // (D37) -- the only rewrite this router performs, and the only reason
        // it can learn anything from a real engine.
        up.body = in.upstream_body.empty() ? client_req.body : in.upstream_body;
        up.connect_timeout_ms = 5000;
        up.read_timeout_ms = request_timeout_ms_;
        up.headers.set("Content-Type",
                       client_req.headers.get("Content-Type", "application/json"));
        up.headers.set("Accept", client_req.headers.get("Accept", "*/*"));
        if (!node_token_.empty() && !cfg->dispatch_direct_to_engine)
            up.headers.set("Authorization", "Bearer " + node_token_);

        up.keep_going = [&res, &client_gone] {
            if (!res.alive()) client_gone = true;
            return res.alive();
        };

        up.on_headers = [&](int status, const http::Headers& headers) {
            a.upstream_status = status;
            if (status != 200) {
                // Nothing has reached the client yet, so this is still
                // retryable. Read the body so the error text can be reported.
                return true;
            }
            http::Headers out_headers;
            out_headers.set("Content-Type",
                            headers.get("Content-Type", "application/json"));
            out_headers.set("Cache-Control", "no-cache");
            out_headers.set("X-RouteFlow-Node", r.node_id);
            out_headers.set("X-RouteFlow-Job", job_id);
            out_headers.set("X-RouteFlow-Warm", r.was_resident ? "1" : "0");
            if (!res.begin(200, out_headers)) return false;
            a.sent_to_client = true;
            return true;
        };

        up.on_chunk = [&](const char* data, size_t n) {
            if (a.ts_first_byte_ms == 0) {
                a.ts_first_byte_ms = now_ms();
                // The request now contends for decode throughput, which is the
                // distinction §6.2 prices. Also release the VRAM reservation:
                // if the model needed loading, it is loaded by now.
                state_.note_decoding(r.token);
                if (r.will_load) state_.note_load_done(r.token);
            }
            tail.append(data, n);
            if (a.upstream_status != 200) return true;  // buffering an error body
            return res.write(data, n);
        };

        a.ts_dispatched_ms = now_ms();

        http::ClientResponse up_res;
        std::string up_err;
        const bool transport_ok = http::perform(up, up_res, &up_err);
        a.ts_done_ms = now_ms();

        if (a.sent_to_client) res.end();

        // --- classify the outcome (§6.4) ------------------------------------
        if (client_gone) {
            a.outcome = Outcome::ClientAbort;
            a.error = "client disconnected";
        } else if (!transport_ok) {
            a.outcome = a.sent_to_client ? Outcome::StreamFailed : Outcome::DispatchFailed;
            a.error = up_err.empty() ? "upstream transport failure" : up_err;
        } else if (a.upstream_status != 200) {
            a.outcome = Outcome::DispatchFailed;
            a.error = "upstream status " + std::to_string(a.upstream_status) + ": " +
                      tail.str().substr(0, 300);
        } else {
            a.ok = true;
            a.outcome = Outcome::Ok;
        }

        if (a.ok || a.upstream_status == 200) harvest_from_tail(tail.str(), a);

        // A resident model has a load time of exactly zero — that is a
        // measurement, not a gap. An engine that reported nothing leaves it
        // unknown, which is null in the trace, never zero (§10).
        if (!a.has_load_ms && r.was_resident) {
            a.has_load_ms = true;
            a.load_ms = 0;
        }

        // --- record ---------------------------------------------------------
        TraceRecord rec;
        rec.job_id = job_id;
        rec.retry_of = previous_job_id;
        rec.ts_received_ms = ts_received;
        rec.ts_dispatched_ms = a.ts_dispatched_ms;
        rec.ts_first_token_ms = a.ts_first_byte_ms;
        rec.ts_done_ms = a.ts_done_ms;

        rec.model = in.features.model;
        rec.role_hint = in.features.role_hint;
        rec.num_ctx = in.features.num_ctx;
        rec.stream = in.features.stream;

        rec.policy = r.policy_name;
        rec.cost_model = r.cost_model_name;
        rec.node_id = r.node_id;
        rec.was_resident = r.was_resident;
        rec.footprint_bytes = r.footprint_bytes;

        rec.prompt_tokens_est = in.features.prompt_tokens;
        rec.has_prompt_tokens_actual = a.has_prompt_tokens;
        rec.prompt_tokens_actual = a.prompt_tokens;
        rec.has_output_tokens = a.has_output_tokens;
        rec.output_tokens = a.output_tokens;

        // The caller's cap only if the model produced nothing of its own; a
        // capped request predicts the cap, so this changes no capped result.
        rec.predicted_output_tokens = in.features.predicted_output_tokens;
        rec.predicted_output_sigma = in.features.predicted_output_sigma;
        if (const Candidate* w = r.decision.winner()) {
            rec.predicted_total_ms = w->est.total_ms();
            rec.predicted_sigma_ms = w->est.sigma_ms;
            if (w->est.predicted_output_tokens > 0) {
                rec.predicted_output_tokens = w->est.predicted_output_tokens;
                rec.predicted_output_sigma = w->est.predicted_output_sigma;
            }
        }

        // Router-side overhead only. Engine-side queueing is not observable and
        // lands in ttft — see the prefill caveat in docs/TRACE-SCHEMA.md.
        rec.queue_wait_ms = static_cast<double>(a.ts_dispatched_ms - ts_received);
        rec.has_load_ms = a.has_load_ms;
        rec.load_ms = a.load_ms;
        // Time-to-first-token only exists for a stream. On a buffered reply the
        // first byte IS the whole answer, so recording ttft == total would put
        // a value in the TTFT distribution that describes something else
        // entirely. Unknown is null (§10), not a number that happens to parse.
        rec.has_ttft = in.features.stream && a.ts_first_byte_ms != 0;
        rec.ttft_ms = rec.has_ttft
                          ? static_cast<double>(a.ts_first_byte_ms - a.ts_dispatched_ms)
                          : 0;
        rec.total_ms = static_cast<double>(a.ts_done_ms - a.ts_dispatched_ms);

        rec.inflight_at_dispatch = r.inflight_at_dispatch;
        rec.concurrent_decoders_at_dispatch = r.concurrent_decoders_at_dispatch;
        rec.telemetry_ok_at_dispatch = r.telemetry_ok;
        rec.gpu_util_at_dispatch = r.gpu_util;
        rec.vram_free_at_dispatch = r.vram_free;
        rec.telemetry_backend = r.telemetry_backend;

        rec.evicted = r.would_evict;
        rec.decided_by = r.decision.decided_by;
        rec.has_margin = r.decision.has_runner_up;
        rec.margin_ms = r.decision.margin_ms;
        rec.candidates = r.decision.candidates;

        rec.outcome = a.outcome;
        rec.error = a.error;

        state_.complete(r.token, rec);
        trace_.append(rec);

        if (a.ok || a.outcome == Outcome::ClientAbort) return;

        // Anything already written to the client cannot be taken back, so a
        // stream that broke mid-flight is terminal (§6.4).
        if (a.sent_to_client) {
            RF_WARN("job %s: stream failed on '%s' after first byte: %s",
                    job_id.c_str(), r.node_id.c_str(), a.error.c_str());
            return;
        }

        RF_WARN("job %s: dispatch to '%s' failed before first byte (%s)%s",
                job_id.c_str(), r.node_id.c_str(), a.error.c_str(),
                attempt_no + 1 < kMaxAttempts ? "; retrying elsewhere" : "");
        tried.push_back(r.node_id);
        previous_job_id = job_id;
    }

    // Both attempts failed before sending anything.
    if (!res.responded())
        res.send_error(502, "dispatch_failed",
                       "every candidate node failed to serve the request");
}

}  // namespace rf
