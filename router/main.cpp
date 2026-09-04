// routeflow-router — the scheduler (ARCHITECTURE §4.2).
//
//   POST /v1/chat/completions   OpenAI-compatible ingest
//   POST /api/chat              Ollama-native ingest
//   POST /api/generate          Ollama-native ingest
//   GET  /v1/models             union of models on disk across the cluster
//   GET  /                      static console (ui/cluster.html)
//   GET  /snapshot              collector envelope (NoteFlow and friends)
//   GET  /api/nodes             live cluster state (UI)
//   GET  /api/jobs              recent trace records (UI)
//   GET  /events                SSE: node state + job completions (UI)
//   GET  /admin/stats           ledger, counters, active policy
//   POST /admin/policy          runtime policy switch (§5)
//   GET  /health                liveness, no auth

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
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
        "  --http.token SECRET        bearer token required from clients;\n"
        "                             gates /snapshot too, with no exemption\n"
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

    // Summary over the retained window, for the collector envelope. Not a new
    // measurement path — it counts records we already keep.
    //
    // D8's discipline applies across the wire too: a percentile of an empty set
    // is not zero, it is unknown, and a consumer that draws "0 ms" from it would
    // be acting on a number nobody measured.
    rf::Json summary_json() const {
        std::lock_guard<std::mutex> lock(mu_);
        rf::Json j = rf::Json::object();
        j["window"] = rf::Json(capacity_);
        j["total"] = rf::Json(records_.size());

        size_t failed = 0, cold = 0;
        std::vector<double> totals;
        totals.reserve(records_.size());
        for (const auto& r : records_) {
            if (r.outcome != rf::Outcome::Ok) {
                ++failed;
                continue;
            }
            if (!r.was_resident) ++cold;
            totals.push_back(r.total_ms);
        }
        j["failed"] = rf::Json(failed);
        j["cold_starts"] = rf::Json(cold);

        if (totals.empty()) {
            j["p50_ms"] = rf::Json();
            j["p95_ms"] = rf::Json();
        } else {
            std::sort(totals.begin(), totals.end());
            auto pct = [&totals](double q) {
                const double pos = (totals.size() - 1) * q;
                const size_t lo = static_cast<size_t>(pos);
                const size_t hi = std::min(lo + 1, totals.size() - 1);
                return totals[lo] + (totals[hi] - totals[lo]) * (pos - lo);
            };
            j["p50_ms"] = rf::Json(pct(0.5));
            j["p95_ms"] = rf::Json(pct(0.95));
        }
        return j;
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

// --- static UI ---------------------------------------------------------------

// The console is four standalone pages linked by relative hrefs, so they are
// served flat out of one directory.
//
// These files carry no cluster data — only the design's placeholder fixtures —
// and the binding layer fetches the real state from /api/*, which stays behind
// the bearer token. So the shell is served without auth, which is what lets a
// browser load it at all: a navigation cannot carry an Authorization header.
// See the note in docs/design/ about what that means when a token is set.
const char* content_type_for(const std::string& name) {
    auto ends_with = [&name](const char* suffix) {
        const size_t n = std::strlen(suffix);
        return name.size() >= n && name.compare(name.size() - n, n, suffix) == 0;
    };
    if (ends_with(".html")) return "text/html; charset=utf-8";
    if (ends_with(".js")) return "application/javascript; charset=utf-8";
    if (ends_with(".css")) return "text/css; charset=utf-8";
    if (ends_with(".svg")) return "image/svg+xml";
    if (ends_with(".json")) return "application/json";
    if (ends_with(".woff2")) return "font/woff2";
    if (ends_with(".png")) return "image/png";
    return "application/octet-stream";
}

// True only for a plain basename: no separator, no dot-dot, no leading dot.
// The router serves whatever is in the UI directory, so the guard is on the
// name rather than on a resolved path — there is no way to escape a directory
// you can never name your way out of.
bool safe_ui_name(const std::string& name) {
    if (name.empty() || name.size() > 128) return false;
    if (name.front() == '.') return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return name.find("..") == std::string::npos;
}

// The cluster view the console reads, from both /api/nodes and the SSE stream.
//
// It exists because those two were briefly different: the route merged the
// ledger's counts and the stream did not, so `inflight` showed a number on load
// and then vanished on the first push. One builder, one shape.
rf::Json cluster_json(const rf::NodeRegistry& registry, const rf::RouterState& state) {
    rf::Json nodes = registry.to_json();
    const rf::Json ledger = state.ledger_counts_json();
    for (size_t i = 0; i < nodes.size(); ++i) {
        rf::Json& n = nodes.at(i);
        // The console shows the ledger's in-flight count, not the engine's
        // self-report: the ledger is what the scheduler actually reasoned
        // about, while inflight_reported arrives a poll interval late (§6.3).
        const rf::Json& acc = ledger[n["id"].as_str()];
        n["inflight"] = rf::Json(acc["inflight"].as_u32(0));
        n["decoders"] = rf::Json(acc["decoders"].as_u32(0));
        n["reserved_vram_bytes"] = rf::Json(acc["reserved_vram_bytes"].as_u64(0));
    }
    return nodes;
}

// Which models are actually resident, and where.
//
// A node whose engine cannot report residency is NOT reported as holding
// nothing — that would be the same lie as writing 0 for an unmeasured value
// (§10, D3). It is named separately so a consumer can render it as unknown.
rf::Json warm_models_json(const std::vector<rf::NodeState>& nodes,
                          rf::Json& residency_unknown) {
    std::map<std::string, std::vector<std::string>> by_model;
    residency_unknown = rf::Json::array();

    for (const auto& n : nodes) {
        if (!n.engine_healthy) continue;
        if (!n.residency_known) {
            residency_unknown.push_back(rf::Json(n.id));
            continue;
        }
        for (const auto& m : n.models_resident) by_model[m.name].push_back(n.id);
    }

    rf::Json out = rf::Json::array();
    for (const auto& kv : by_model) {
        rf::Json e = rf::Json::object();
        e["model"] = rf::Json(kv.first);
        rf::Json on = rf::Json::array();
        for (const auto& id : kv.second) on.push_back(rf::Json(id));
        e["nodes"] = std::move(on);
        out.push_back(std::move(e));
    }
    return out;
}

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
    // Empty disables static serving entirely, for a router that only routes.
    const std::string ui_dir = cfg.get_str("ui.dir", "ui");

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
        // The static console shell, before the token gate — see the note on
        // content_type_for. It contains no cluster data; everything real comes
        // from /api/*, which is gated below.
        if (req.method == "GET" && !ui_dir.empty()) {
            std::string name;
            if (req.path == "/") name = "cluster.html";
            else if (req.path.find('/', 1) == std::string::npos) name = req.path.substr(1);

            if (!name.empty() && safe_ui_name(name)) {
                std::string body;
                if (rf::read_file(ui_dir + "/" + name, body)) {
                    res.send(200, content_type_for(name), body);
                    return;
                }
            }
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
            res.send_json(200, cluster_json(registry, state).dump());
            return;
        }

        // Collector envelope. RouteFlow publishes the read state it already
        // has under a shared wrapper; it adds no measurement and opens no new
        // path to the control endpoints, which stay exactly where they are.
        //
        // The envelope version is the envelope's own. It is deliberately not
        // tied to the trace schema version (docs/TRACE-SCHEMA.md): the two have
        // separate lifetimes, and a trace bump to v2 leaves this at 1.
        if (req.method == "GET" && req.path == "/snapshot") {
            const std::vector<rf::NodeState> nodes = registry.snapshot();

            rf::Json collector = rf::Json::object();
            // Identity, not configuration: consumers key their card schemas on
            // this slug, so it is a fixed string and no flag can change it.
            collector["id"] = rf::Json("routeflow");
            collector["kind"] = rf::Json("inference");
            collector["version"] = rf::Json(rf::kVersion);
            // What we are bound to. A router listening on 0.0.0.0 reports that
            // literally rather than guessing which interface a reader reached
            // it on — an invented address is worse than an obviously generic one.
            collector["endpoint"] =
                rf::Json("http://" + bind + ":" + std::to_string(server.port()));

            rf::Json snapshot = rf::Json::object();
            snapshot["v"] = rf::Json(1);
            snapshot["collector"] = std::move(collector);
            snapshot["now_ns"] = rf::Json(rf::now_ns());

            // Body sits at the same level as the envelope, not nested.
            snapshot["nodes"] = cluster_json(registry, state);
            snapshot["jobs_recent"] = history.summary_json();
            rf::Json residency_unknown;
            snapshot["models_warm"] = warm_models_json(nodes, residency_unknown);
            if (residency_unknown.size() > 0)
                snapshot["residency_unknown_nodes"] = std::move(residency_unknown);
            snapshot["policy"] = rf::Json(state.policy_name());
            snapshot["cost_model"] = rf::Json(state.cost_model_name());

            res.send_json(200, snapshot.dump());
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
            if (!res.sse("nodes", cluster_json(registry, state).dump())) return;

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
                    if (!res.sse("nodes", cluster_json(registry, state).dump())) break;
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
