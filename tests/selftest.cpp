// RouteFlow — self-test for /common.
//
// No test framework (ARCHITECTURE §3: no third-party libraries). Each check
// prints and counts; the process exit code is the number of failures.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"
#include "common/http.h"
#include "common/json.h"
#include "common/types.h"
#include "common/util.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void check_eq(const std::string& got, const std::string& want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s\n        got:  %s\n        want: %s\n",
                    what.c_str(), got.c_str(), want.c_str());
    }
}

void section(const char* name) { std::printf("\n%s\n", name); }

// --- json -------------------------------------------------------------------

void test_json() {
    section("json");

    rf::Json j;
    std::string err;
    check(rf::Json::parse(R"({"a":1,"b":[true,null,"x"],"c":{"d":-2.5}})", j, &err),
          "parse object: " + err);
    check(j["a"].as_i64() == 1, "a == 1");
    check(j["b"].size() == 3, "b has 3 items");
    check(j["b"].at(0).as_bool(), "b[0] is true");
    check(j["b"].at(1).is_null(), "b[1] is null");
    check(j["c"]["d"].as_num() == -2.5, "c.d == -2.5");

    // Missing keys yield null rather than throwing (trace rule 4).
    check(j["nope"].is_null(), "missing key is null");
    check(j["nope"]["deeper"].is_null(), "missing nested key is null");
    check(j["b"].at(99).is_null(), "out-of-range index is null");

    // null and 0 are distinct: load_ms:0 means resident, null means unmeasured.
    check(!rf::Json(0).is_null() && rf::Json().is_null(), "null is not zero");

    // Integers above 2^53 must survive exactly. A double cannot hold them, and
    // an epoch timestamp in nanoseconds is ~1.8e18 — stored as a double it both
    // loses precision and prints as 1.79e+18, which is not what a consumer
    // expecting an integer will parse.
    const int64_t ns = 1788463371247802432LL;
    check_eq(rf::Json(ns).dump(), "1788463371247802432",
             "a nanosecond timestamp prints as an integer, not in exponent form");
    rf::Json big;
    check(rf::Json::parse(rf::Json(ns).dump(), big, &err) && big.as_i64() == ns,
          "and round-trips through the parser unchanged");
    check(rf::Json(9007199254740993LL).dump() == "9007199254740993",
          "2^53+1 survives, which a double could not represent");
    check_eq(rf::Json(static_cast<uint64_t>(21903073280ULL)).dump(), "21903073280",
             "byte counts stay exact");

    check_eq(rf::Json(1.0).dump(), "1", "integral double prints without .0");
    check_eq(rf::Json(0.1).dump(), "0.1", "0.1 round-trips shortest");
    check_eq(rf::Json(1e300).dump(), "1e+300", "large double");

    rf::Json esc;
    check(rf::Json::parse("\"a\\nb\\u00e7\\ud83d\\ude00\"", esc, &err), "parse escapes");
    check_eq(esc.as_str(), "a\nb\xc3\xa7\xf0\x9f\x98\x80", "escapes decode to UTF-8");
    rf::Json reparsed;
    check(rf::Json::parse(esc.dump(), reparsed, &err) && reparsed.as_str() == esc.as_str(),
          "escaped string round-trips");

    // Objects keep insertion order so trace records stay readable.
    rf::Json o = rf::Json::object();
    o["z"] = rf::Json(1);
    o["a"] = rf::Json(2);
    check_eq(o.dump(), R"({"z":1,"a":2})", "object preserves insertion order");

    check(!rf::Json::parse("{\"a\":1,}", j, &err), "trailing comma rejected");
    check(!rf::Json::parse("[1,2", j, &err), "unterminated array rejected");
    check(!rf::Json::parse("", j, &err), "empty input rejected");
    check(!rf::Json::parse("{} junk", j, &err), "trailing content rejected");
}

// --- util -------------------------------------------------------------------

void test_util() {
    section("util");

    const int64_t t = 1788445267412LL;  // 2026-09-03T14:21:07.412Z
    check_eq(rf::iso8601(t), "2026-09-03T14:21:07.412Z", "iso8601 format");
    check(rf::parse_iso8601(rf::iso8601(t)) == t, "iso8601 round-trips");
    check(rf::parse_iso8601("not a timestamp") == 0, "bad timestamp yields 0");

    // ULIDs must sort in creation order: the trace file is read chronologically.
    std::vector<std::string> ids;
    for (int i = 0; i < 500; ++i) ids.push_back(rf::ulid());
    bool sorted = true, unique = true;
    for (size_t i = 1; i < ids.size(); ++i) {
        if (ids[i] <= ids[i - 1]) sorted = false;
        if (ids[i] == ids[i - 1]) unique = false;
    }
    check(ids[0].size() == 26, "ulid is 26 chars");
    check(sorted, "ulids are monotonic within a process");
    check(unique, "ulids are unique");

    check(rf::estimate_tokens("") == 0, "empty prompt is 0 tokens");
    const uint32_t latin = rf::estimate_tokens(std::string(400, 'a'));
    check(latin >= 90 && latin <= 110, "400 latin bytes ~ 100 tokens");
    // Turkish diacritics are two-byte: the estimate must not collapse to bytes/4.
    check(rf::estimate_tokens("çğışöü") > rf::estimate_tokens("cgisou"),
          "accented text estimates higher than plain ascii of same length");

    check_eq(rf::trim("  x \n"), "x", "trim");
    check(rf::iequals("Content-Type", "content-type"), "iequals");
    check(rf::split("a,b,,c", ',').size() == 4, "split keeps empty fields");
}

// --- config -----------------------------------------------------------------

void test_config() {
    section("config");

    rf::Config cfg;
    const char* argv[] = {"prog", "--http.port", "9100", "--scoring.evict_weight",
                          "0.25", "--http.token=secret", "--verbose=true", "run.json"};
    cfg.apply_args(8, const_cast<char**>(argv));

    check(cfg.get_u32("http.port") == 9100, "dotted int override");
    check(std::fabs(cfg.get_num("scoring.evict_weight") - 0.25) < 1e-12,
          "dotted float override");
    check_eq(cfg.get_str("http.token"), "secret", "--key=value form");
    check(cfg.get_bool("verbose"), "--flag=true parses as boolean");
    check(cfg.positionals().size() == 1 && cfg.positionals()[0] == "run.json",
          "positional captured after an =-form flag");
    check(!cfg.has("missing.key"), "missing key absent");
    check(cfg.get_u32("missing.key", 7) == 7, "default returned for missing key");

    // The documented ambiguity: a bare flag swallows a following positional.
    // Asserted rather than left as a surprise, because config parsing that
    // silently eats an argument is a bad way to lose an afternoon.
    rf::Config bare;
    const char* argv2[] = {"prog", "--verbose", "run.json"};
    bare.apply_args(3, const_cast<char**>(argv2));
    check_eq(bare.get_str("verbose"), "run.json", "bare flag consumes next token");
    check(bare.positionals().empty(), "...leaving no positional");

    rf::Config trailing;
    const char* argv3[] = {"prog", "--verbose"};
    trailing.apply_args(2, const_cast<char**>(argv3));
    check(trailing.get_bool("verbose"), "bare flag at end of args is true");
}

// --- trace ------------------------------------------------------------------

void test_trace_record() {
    section("trace record");

    rf::TraceRecord r;
    r.job_id = rf::ulid();
    r.ts_received_ms = 1788445267412LL;
    r.ts_dispatched_ms = r.ts_received_ms + 43;
    r.ts_first_token_ms = r.ts_received_ms + 4353;
    r.ts_done_ms = r.ts_received_ms + 7963;
    r.model = "qwen4:12b";
    r.role_hint = "planner";
    r.num_ctx = 8192;
    r.stream = true;
    r.policy = "roundrobin-v1";
    r.cost_model = "static-v1";
    r.node_id = "jetson-01";
    r.was_resident = false;
    r.prompt_tokens_est = 1795;
    r.has_prompt_tokens_actual = true;
    r.prompt_tokens_actual = 1842;
    r.has_output_tokens = true;
    r.output_tokens = 312;
    r.predicted_output_tokens = 280;
    r.predicted_output_sigma = 96.f;
    r.predicted_total_ms = 8100;
    r.predicted_sigma_ms = 1400;
    r.queue_wait_ms = 40;
    r.load_ms = 3980;
    r.has_ttft = true;
    r.ttft_ms = 4310;
    r.total_ms = 7920;
    r.inflight_at_dispatch = 1;
    r.concurrent_decoders_at_dispatch = 1;
    r.telemetry_ok_at_dispatch = true;
    r.gpu_util_at_dispatch = 0.12f;
    r.vram_free_at_dispatch = 21903073280ULL;
    r.telemetry_backend = "nvml";
    r.decided_by = "t_load";
    r.has_margin = true;
    r.margin_ms = 1500;

    rf::Candidate win;
    win.node_id = "jetson-01";
    win.admitted = true;
    win.est.t_queue_ms = 40;
    win.est.t_load_ms = 3980;
    win.est.t_prefill_ms = 180;
    win.est.t_decode_ms = 3900;
    win.est.sigma_ms = 1400;
    win.telemetry_ok = true;
    win.gpu_util = 0.12f;
    win.vram_free_bytes = 21903073280ULL;
    win.inflight = 1;

    rf::Candidate rejected;
    rejected.node_id = "laptop-01";
    rejected.admitted = false;
    rejected.reason = rf::AdmitReason::InsufficientVram;
    rejected.vram_free_bytes = 1073741824ULL;

    r.candidates = {win, rejected};

    const std::string line = r.to_json().dump();
    check(line.find('\n') == std::string::npos, "record is a single JSONL line");
    check(line.find("\"v\":1") != std::string::npos, "schema version present");

    rf::Json parsed;
    std::string err;
    check(rf::Json::parse(line, parsed, &err), "record re-parses: " + err);

    // Not-admitted candidates carry null predictions, not zeros.
    check(parsed["candidates"].at(1)["predicted_total_ms"].is_null(),
          "rejected candidate has null prediction");
    check(parsed["candidates"].at(1)["vram_free"].as_u64() == 1073741824ULL,
          "rejected candidate still carries its state (counterfactuals)");

    rf::TraceRecord back;
    check(rf::TraceRecord::from_json(parsed, back, &err), "record parses back: " + err);
    check(back.job_id == r.job_id, "job_id round-trips");
    check(back.ts_done_ms == r.ts_done_ms, "timestamps round-trip");
    check(back.total_ms == r.total_ms, "total_ms round-trips");
    check(back.candidates.size() == 2, "candidates round-trip");
    check(back.has_prompt_tokens_actual, "actual prompt tokens present");
    check(back.telemetry_ok_at_dispatch, "telemetry flag recovered from null-ness");

    // The learning derivation of §7 must close on a real record.
    const double t_prefill = back.ttft_ms - back.load_ms - back.queue_wait_ms;
    const double t_decode = back.total_ms - back.ttft_ms;
    check(t_prefill > 0, "derived prefill time is positive");
    check(t_decode > 0, "derived decode time is positive");

    // A record with no telemetry must serialise nulls, not zeros.
    rf::TraceRecord dark = r;
    dark.telemetry_ok_at_dispatch = false;
    rf::Json dj;
    check(rf::Json::parse(dark.to_json().dump(), dj, &err), "dark record parses");
    check(dj["gpu_util_at_dispatch"].is_null(), "absent telemetry is null, not 0");
    check(dj["vram_free_at_dispatch"].is_null(), "absent vram is null, not 0");

    // Version gate.
    rf::Json unversioned;
    rf::Json::parse("{\"job_id\":\"x\"}", unversioned, &err);
    rf::TraceRecord ignored;
    check(!rf::TraceRecord::from_json(unversioned, ignored, &err),
          "record without a version is rejected");
}

// --- node state -------------------------------------------------------------

void test_node_state() {
    section("node state");

    rf::NodeState n;
    n.id = "desktop-01";
    n.gpu_name = "NVIDIA GeForce RTX 4060 Laptop GPU";
    n.telemetry_backend = "nvml";
    n.telemetry_ok = true;
    n.vram_total_bytes = 8583086080ULL;
    n.vram_free_bytes = 3221225472ULL;
    n.gpu_util = 0.81f;
    n.power_watts = 61.5f;
    n.power_cap_watts = 80.f;
    n.temperature_c = 67.f;
    n.engine = rf::EngineKind::Ollama;
    n.engine_healthy = true;
    n.engine_slots = 4;
    n.models_on_disk = {"qwen4:12b", "llama4:8b"};
    n.models_resident = {{"llama4:8b", 5100000000ULL, 1788445260000LL}};
    n.model_disk_bytes = {{"qwen4:12b", 7400000000ULL}};
    n.sampled_at_ms = 1788445267000LL;

    rf::Json j;
    std::string err;
    check(rf::Json::parse(n.to_json().dump(), j, &err), "state serialises: " + err);

    rf::NodeState back;
    check(rf::NodeState::from_json(j, back), "state parses back");
    check(back.id == n.id, "id round-trips");
    check(back.engine == rf::EngineKind::Ollama, "engine kind round-trips");
    check(back.engine_slots == 4, "engine slots round-trip");
    check(back.vram_free_bytes == n.vram_free_bytes, "free vram round-trips");

    // On-disk and resident are different sets. That distinction is the project.
    check(back.has_on_disk("qwen4:12b") && !back.is_resident("qwen4:12b"),
          "on-disk model is not automatically resident");
    check(back.is_resident("llama4:8b"), "resident model recognised");
    check(back.resident_bytes("llama4:8b") == 5100000000ULL, "resident bytes kept");
    check(back.disk_bytes("qwen4:12b") == 7400000000ULL, "disk size kept (seeds footprint)");

    rf::NodeState dark;
    dark.id = "jetson-01";
    dark.telemetry_ok = false;
    rf::Json dj;
    rf::Json::parse(dark.to_json().dump(), dj, &err);
    check(dj["gpu_util"].is_null(), "node without telemetry reports null util");

    rf::NodeState anon;
    check(!rf::NodeState::from_json(dj["models_on_disk"], anon), "non-object rejected");
}

// --- http -------------------------------------------------------------------

void test_http() {
    section("http");

    rf::http::Server server;
    std::atomic<int> hits{0};

    server.set_worker_threads(4);
    server.set_handler([&](const rf::http::Request& req, rf::http::Responder& res) {
        ++hits;
        if (req.path == "/echo") {
            res.send(200, "text/plain", req.method + " " + req.body);
        } else if (req.path == "/query") {
            res.send(200, "text/plain", req.param("name") + "|" + req.param("missing", "-"));
        } else if (req.path == "/stream") {
            rf::http::Headers h;
            h.set("Content-Type", "text/plain");
            res.begin(200, h);
            for (int i = 0; i < 5; ++i) res.write("chunk" + std::to_string(i) + "\n");
            res.end();
        } else if (req.path == "/events") {
            rf::http::Headers h;
            h.set("Content-Type", "text/event-stream");
            h.set("Cache-Control", "no-cache");
            res.begin(200, h);
            res.sse("state", "{\"a\":1}");
            res.sse("", "line1\nline2");
            res.end();
        } else if (req.path == "/slow") {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            res.send(200, "text/plain", "late");
        } else {
            res.send_error(404, "not_found", "no route for " + req.path);
        }
    });

    std::string err;
    if (!server.listen("127.0.0.1", 0, &err)) {
        check(false, "server listen: " + err);
        return;
    }
    const uint16_t port = server.port();
    std::thread runner([&server] { server.run(); });

    auto base = [&] {
        rf::http::ClientRequest r;
        r.host = "127.0.0.1";
        r.port = port;
        r.connect_timeout_ms = 2000;
        r.read_timeout_ms = 5000;
        return r;
    };

    {   // buffered POST round-trip
        rf::http::ClientRequest req = base();
        req.method = "POST";
        req.path = "/echo";
        req.body = "hello body";
        req.headers.set("Content-Type", "text/plain");
        rf::http::ClientResponse res;
        check(rf::http::perform(req, res, &err), "POST /echo: " + err);
        check(res.status == 200, "POST /echo status 200");
        check_eq(res.body, "POST hello body", "POST /echo body");
    }

    {   // query parsing and defaults
        rf::http::ClientRequest req = base();
        req.path = "/query?name=jetson%2D01&other=2";
        rf::http::ClientResponse res;
        check(rf::http::perform(req, res, &err), "GET /query: " + err);
        check_eq(res.body, "jetson-01|-", "query params decoded, default applied");
    }

    {   // 404 shape
        rf::http::ClientRequest req = base();
        req.path = "/nope";
        rf::http::ClientResponse res;
        check(rf::http::perform(req, res, &err), "GET /nope: " + err);
        check(res.status == 404, "unknown route is 404");
        rf::Json j;
        check(rf::Json::parse(res.body, j, &err) && j["error"]["type"].as_str() == "not_found",
              "error body is JSON in the OpenAI shape");
    }

    {   // chunked streaming, chunk-by-chunk (this is how TTFT is measured)
        rf::http::ClientRequest req = base();
        req.path = "/stream";
        int chunks = 0;
        std::string all;
        req.on_chunk = [&](const char* d, size_t n) {
            ++chunks;
            all.append(d, n);
            return true;
        };
        rf::http::ClientResponse res;
        check(rf::http::perform(req, res, &err), "GET /stream: " + err);
        check(res.status == 200, "stream status 200");
        check(chunks >= 5, "chunks arrive separately, not coalesced into one");
        check_eq(all, "chunk0\nchunk1\nchunk2\nchunk3\nchunk4\n", "stream payload intact");
        check(res.body.empty(), "streamed body is not also buffered");
    }

    {   // SSE framing
        rf::http::ClientRequest req = base();
        req.path = "/events";
        std::string all;
        req.on_chunk = [&](const char* d, size_t n) { all.append(d, n); return true; };
        rf::http::ClientResponse res;
        check(rf::http::perform(req, res, &err), "GET /events: " + err);
        check(all.find("event: state\ndata: {\"a\":1}\n\n") != std::string::npos,
              "named SSE event framed correctly");
        check(all.find("data: line1\ndata: line2\n\n") != std::string::npos,
              "multi-line SSE payload gets one data: per line");
    }

    {   // abort from the client side: the router must be able to stop a stream
        rf::http::ClientRequest req = base();
        req.path = "/stream";
        int seen = 0;
        req.on_chunk = [&](const char*, size_t) { ++seen; return false; };
        rf::http::ClientResponse res;
        check(rf::http::perform(req, res, &err), "aborted stream returns cleanly");
        check(res.aborted, "abort is reported, not silently successful");
        check(seen == 1, "no further chunks after abort");
    }

    {   // connect failure must be an error, not a hang
        rf::http::ClientRequest req = base();
        req.port = 1;  // nothing listens here
        req.connect_timeout_ms = 500;
        rf::http::ClientResponse res;
        check(!rf::http::perform(req, res, &err), "connect to dead port fails");
        check(!err.empty(), "failure carries a message");
    }

    {   // concurrent requests: the pool must not serialise them
        const int64_t start = rf::mono_ms();
        std::vector<std::thread> threads;
        std::atomic<int> ok{0};
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&] {
                rf::http::ClientRequest req = base();
                req.path = "/slow";
                rf::http::ClientResponse res;
                std::string e;
                if (rf::http::perform(req, res, &e) && res.status == 200) ++ok;
            });
        }
        for (auto& t : threads) t.join();
        const int64_t elapsed = rf::mono_ms() - start;
        check(ok == 4, "4 concurrent requests all succeeded");
        check(elapsed < 1200, "concurrent requests overlap (took " +
                                  std::to_string(elapsed) + " ms, serial would be 1600)");
    }

    check(hits >= 10, "server saw every request");

    server.stop();
    runner.join();
}

}  // namespace


int main() {
    rf::http::init_process();
    rf::log_set_level(rf::LogLevel::Warn);

    test_json();
    test_util();
    test_config();
    test_trace_record();
    test_node_state();
    test_http();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
