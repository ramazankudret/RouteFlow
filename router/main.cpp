// routeflow-router — the scheduler (ARCHITECTURE §4.2).
//
//   POST /v1/chat/completions   OpenAI-compatible ingest
//   POST /api/chat              Ollama-native ingest
//   POST /api/generate          Ollama-native ingest
//   GET  /v1/models             union of models on disk across the cluster
//   GET  /api/nodes             live cluster state (UI)
//   GET  /api/jobs              recent trace records (UI)
//   GET  /events                SSE: node state + job completions (UI)
//   GET  /admin/stats           ledger, counters, active policy
//   POST /admin/policy          runtime policy switch (§5)
//   GET  /health                liveness, no auth

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "common/config.h"
#include "common/http.h"
#include "common/json.h"
#include "common/util.h"
#include "router/core/dispatch.h"
#include "router/core/interfaces.h"
#include "router/core/registry.h"
#include "router/core/router_state.h"
#include "router/core/trace_writer.h"

namespace {

rf::http::Server* g_server = nullptr;
std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true);
    if (g_server) g_server->stop();
}

void usage() {
    std::fprintf(stderr,
        "routeflow-router - warmth-aware inference scheduler\n"
        "\n"
        "  routeflow-router --config router.json [overrides]\n"
        "\n"
        "  --http.bind ADDR           default 127.0.0.1; 0.0.0.0 requires --http.token\n"
        "  --http.port N              default 8970\n"
        "  --http.token SECRET        bearer token required from clients\n"
        "  --nodes PATH               nodes.json (default: nodes.json)\n"
        "  --node.token SECRET        bearer token presented to agents\n"
        "  --trace PATH               trace file (default: trace.jsonl)\n"
        "  --policy NAME              roundrobin-v1 (default)\n"
        "  --poll_ms N                node poll interval (default 1000)\n"
        "  --request_timeout_ms N     default 600000 (a cold load can be minutes)\n"
        "  --scoring.evict_weight F   default 0.5\n"
        "  --scoring.node_stale_ms N  default 5000\n"
        "  --log.level LEVEL          trace|debug|info|warn|error\n");
}

std::string config_path_from_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) return argv[i + 1];
        if (rf::starts_with(arg, "--config=")) return arg.substr(9);
    }
    return std::string();
}

bool authorised(const rf::http::Request& req, const std::string& token) {
    if (token.empty()) return true;
    return req.headers.get("Authorization") == "Bearer " + token;
}

// Recent completions, for the UI's job feed. The trace file remains the record
// of truth; this is a window onto it that costs no disk read.
class JobHistory {
public:
    explicit JobHistory(size_t capacity) : capacity_(capacity) {}

    void add(const rf::TraceRecord& record) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            records_.push_back(record);
            while (records_.size() > capacity_) records_.pop_front();
            ++version_;
        }
        changed_.notify_all();
    }

    rf::Json to_json(size_t limit) const {
        std::lock_guard<std::mutex> lock(mu_);
        rf::Json arr = rf::Json::array();
        const size_t start = records_.size() > limit ? records_.size() - limit : 0;
        for (size_t i = records_.size(); i-- > start;)  // newest first
            arr.push_back(records_[i].to_json());
        return arr;
    }

    bool latest(rf::TraceRecord& out) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (records_.empty()) return false;
        out = records_.back();
        return true;
    }

    uint64_t version() const {
        std::lock_guard<std::mutex> lock(mu_);
        return version_;
    }

    uint64_t wait_for_change(uint64_t seen, int timeout_ms) const {
        std::unique_lock<std::mutex> lock(mu_);
        changed_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                          [&] { return version_ != seen; });
        return version_;
    }

    void wake_all() { changed_.notify_all(); }

private:
    mutable std::mutex mu_;
    mutable std::condition_variable changed_;
    std::deque<rf::TraceRecord> records_;
    size_t capacity_;
    uint64_t version_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    rf::http::init_process();

    rf::Config cfg;
    const std::string config_path = config_path_from_args(argc, argv);
    if (!config_path.empty()) {
        std::string err;
        if (!cfg.load_file(config_path, &err)) {
            std::fprintf(stderr, "config: %s\n", err.c_str());
            return 2;
        }
    }
    cfg.apply_args(argc, argv);

    if (cfg.get_bool("help") || cfg.get_bool("h")) {
        usage();
        return 0;
    }
    if (cfg.has("log.level") && !rf::log_set_level(cfg.get_str("log.level"))) {
        std::fprintf(stderr, "unknown log level '%s'\n", cfg.get_str("log.level").c_str());
        return 2;
    }

    const std::string bind = cfg.get_str("http.bind", "127.0.0.1");
    const uint16_t port = static_cast<uint16_t>(cfg.get_u32("http.port", 8970));
    const std::string client_token = cfg.get_str("http.token");
    const std::string node_token = cfg.get_str("node.token");
    const std::string nodes_path = cfg.get_str("nodes", "nodes.json");
    const std::string trace_path = cfg.get_str("trace", "trace.jsonl");
    const int poll_ms = static_cast<int>(cfg.get_u32("poll_ms", 1000));
    const int node_timeout_ms = static_cast<int>(cfg.get_u32("node.timeout_ms", 2000));
    const int request_timeout_ms =
        static_cast<int>(cfg.get_u32("request_timeout_ms", 600000));

    // §10, from the first commit: LAN exposure is opt-in and authenticated.
    if (bind != "127.0.0.1" && client_token.empty()) {
        std::fprintf(stderr,
                     "refusing to bind %s without --http.token.\n"
                     "The router proxies prompts verbatim to every machine in %s; "
                     "exposing it unauthenticated hands that to the network.\n",
                     bind.c_str(), nodes_path.c_str());
        return 2;
    }

    // --- nodes ---------------------------------------------------------------
    rf::NodeRegistry registry;
    {
        std::string text, err;
        if (!rf::read_file(nodes_path, text)) {
            std::fprintf(stderr, "cannot read %s\n", nodes_path.c_str());
            return 2;
        }
        rf::Json j;
        if (!rf::Json::parse(text, j, &err)) {
            std::fprintf(stderr, "%s: %s\n", nodes_path.c_str(), err.c_str());
            return 2;
        }
        // Accept either a bare array or {"nodes": [...]}.
        const rf::Json& arr = j.is_array() ? j : j["nodes"];
        if (!registry.load(arr, &err)) {
            std::fprintf(stderr, "%s: %s\n", nodes_path.c_str(), err.c_str());
            return 2;
        }
    }

    // --- trace ---------------------------------------------------------------
    rf::TraceWriter trace;
    {
        std::string err;
        if (!trace.open(trace_path, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 2;
        }
    }

    JobHistory history(cfg.get_u32("history", 500));
    trace.set_observer([&history](const rf::TraceRecord& r) { history.add(r); });

    // --- policy, cost model, state ------------------------------------------
    const rf::ScoringConfig scoring = rf::ScoringConfig::from_config(cfg);
    rf::RouterState state(registry, rf::make_static_cost_model(scoring),
                          rf::make_round_robin_policy(), scoring);

    const std::string wanted_policy = cfg.get_str("policy", "roundrobin-v1");
    if (!state.set_policy(wanted_policy)) {
        std::fprintf(stderr, "unknown policy '%s'\n", wanted_policy.c_str());
        return 2;
    }

    rf::Dispatcher dispatcher(state, registry, trace, node_token, request_timeout_ms);

    registry.start(node_token, poll_ms, node_timeout_ms);

    // --- http ----------------------------------------------------------------
    rf::http::Server server;
    g_server = &server;
    server.set_worker_threads(static_cast<int>(cfg.get_u32("http.threads", 48)));

    server.set_handler([&](const rf::http::Request& req, rf::http::Responder& res) {
        if (req.path == "/health") {
            res.send_json(200, "{\"ok\":true,\"service\":\"routeflow-router\"}");
            return;
        }
        if (!authorised(req, client_token)) {
            res.send_error(401, "unauthorized", "missing or bad bearer token");
            return;
        }

        // --- ingest ---------------------------------------------------------
        if (req.method == "POST" &&
            (req.path == "/v1/chat/completions" || req.path == "/api/chat" ||
             req.path == "/api/generate")) {
            dispatcher.handle(req, res);
            return;
        }

        // --- model listings -------------------------------------------------
        if (req.method == "GET" && (req.path == "/v1/models" || req.path == "/api/tags")) {
            // The union of what the cluster can serve. Deduplicated: the same
            // model on three nodes is one model to a client.
            std::set<std::string> models;
            for (const auto& n : registry.snapshot())
                for (const auto& m : n.models_on_disk) models.insert(m);

            rf::Json out = rf::Json::object();
            rf::Json arr = rf::Json::array();
            for (const auto& m : models) {
                rf::Json e = rf::Json::object();
                if (req.path == "/v1/models") {
                    e["id"] = rf::Json(m);
                    e["object"] = rf::Json("model");
                    e["owned_by"] = rf::Json("routeflow");
                } else {
                    e["name"] = rf::Json(m);
                    e["model"] = rf::Json(m);
                }
                arr.push_back(std::move(e));
            }
            if (req.path == "/v1/models") {
                out["object"] = rf::Json("list");
                out["data"] = std::move(arr);
            } else {
                out["models"] = std::move(arr);
            }
            res.send_json(200, out.dump());
            return;
        }

        // --- UI data --------------------------------------------------------
        if (req.method == "GET" && req.path == "/api/nodes") {
            res.send_json(200, registry.to_json().dump());
            return;
        }
        if (req.method == "GET" && req.path == "/api/jobs") {
            const uint32_t limit =
                static_cast<uint32_t>(std::atoi(req.param("limit", "100").c_str()));
            res.send_json(200, history.to_json(limit > 0 ? limit : 100).dump());
            return;
        }
        if (req.method == "GET" && req.path == "/events") {
            rf::http::Headers h;
            h.set("Content-Type", "text/event-stream");
            h.set("Cache-Control", "no-cache");
            h.set("X-Accel-Buffering", "no");
            if (!res.begin(200, h)) return;

            uint64_t seen = history.version();
            if (!res.sse("nodes", registry.to_json().dump())) return;

            int64_t last_nodes_push = rf::mono_ms();
            while (res.alive() && !g_stop.load()) {
                const uint64_t now = history.wait_for_change(seen, 1000);
                if (now != seen) {
                    seen = now;
                    rf::TraceRecord latest;
                    if (history.latest(latest) && !res.sse("job", latest.to_json().dump()))
                        break;
                }
                // Node telemetry has no change signal here (the registry polls
                // on its own schedule), so push it on a timer.
                if (rf::mono_ms() - last_nodes_push >= 1000) {
                    last_nodes_push = rf::mono_ms();
                    if (!res.sse("nodes", registry.to_json().dump())) break;
                }
            }
            res.end();
            return;
        }

        // --- admin ----------------------------------------------------------
        if (req.method == "GET" && req.path == "/admin/stats") {
            rf::Json j = state.stats_json();
            j["trace_path"] = rf::Json(trace.path());
            j["trace_records_written"] = rf::Json(trace.records_written());
            j["trace_write_errors"] = rf::Json(trace.write_errors());
            res.send_json(200, j.dump(2));
            return;
        }
        if (req.method == "POST" && req.path == "/admin/policy") {
            rf::Json body;
            std::string perr;
            if (!rf::Json::parse(req.body, body, &perr)) {
                res.send_error(400, "invalid_request_error", "bad JSON: " + perr);
                return;
            }
            const std::string name = body["policy"].as_str();
            if (!state.set_policy(name)) {
                res.send_error(400, "unknown_policy",
                               "no policy named '" + name + "'");
                return;
            }
            res.send_json(200, "{\"policy\":\"" + state.policy_name() + "\"}");
            return;
        }

        res.send_error(404, "not_found", "no route for " + req.path);
    });

    std::string err;
    if (!server.listen(bind, port, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        registry.stop();
        return 2;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    RF_INFO("routeflow-router on %s:%u | %zu node(s) | policy %s | cost %s | trace %s",
            bind.c_str(), server.port(), registry.configs().size(),
            state.policy_name().c_str(), state.cost_model_name().c_str(),
            trace_path.c_str());
    if (client_token.empty())
        RF_INFO("no client token set; bound to loopback only");

    server.run();

    g_stop.store(true);
    history.wake_all();
    registry.stop();
    RF_INFO("routeflow-router stopped (%llu trace records written)",
            static_cast<unsigned long long>(trace.records_written()));
    return 0;
}
