// routeflow-agent — one per node. Observes; never decides (ARCHITECTURE §4.1).
//
//   GET  /state    the node's current NodeState
//   GET  /events   SSE stream of the same, pushed on change
//   GET  /health   liveness, no auth
//
// In --simulate mode the process additionally serves the engine endpoints
// itself, so a simulated node is indistinguishable from a real one to the
// router (D16).

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent/engine/engine_probe.h"
#include "agent/engine/residency_limit.h"
#include "agent/sim/sim_node.h"
#include "agent/telemetry/telemetry.h"
#include "common/config.h"
#include "common/http.h"
#include "common/json.h"
#include "common/types.h"
#include "common/util.h"

namespace {

rf::http::Server* g_server = nullptr;
std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true);
    if (g_server) g_server->stop();
}

void usage() {
    std::fprintf(stderr,
        "routeflow-agent - node-side observer\n"
        "\n"
        "  routeflow-agent [--config agent.json] [overrides]\n"
        "\n"
        "Overrides use dotted config keys; any key in the file can be overridden:\n"
        "  --node.id NAME            node identifier (default: hostname)\n"
        "  --http.bind ADDR          default 127.0.0.1; 0.0.0.0 requires --http.token\n"
        "  --http.port N             default 8971\n"
        "  --http.token SECRET       bearer token required on /state and /events\n"
        "  --engine.kind KIND        ollama | lmstudio (default ollama)\n"
        "  --engine.endpoint H:P     default 127.0.0.1:11434\n"
        "  --engine.slots N          parallel slots the engine accepts (default 1)\n"
        "  --telemetry BACKEND       force nvml | tegra | null\n"
        "  --poll_ms N               poll interval (default 1000)\n"
        "  --simulate PROFILE.json   run as a simulated node\n"
        "  --log.level LEVEL         trace|debug|info|warn|error\n");
}

// The agent's view of its node, rebuilt each poll and read by the handlers.
class SharedState {
public:
    void publish(rf::NodeState s) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            state_ = std::move(s);
            ++version_;
        }
        changed_.notify_all();
    }

    rf::NodeState snapshot(uint64_t* version_out = nullptr) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (version_out) *version_out = version_;
        return state_;
    }

    // Blocks until the published version differs from `seen` or the timeout
    // expires; returns the version now current. The timeout is what keeps an
    // idle SSE connection sending periodic keep-alives.
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
    rf::NodeState state_;
    uint64_t version_ = 0;
};

std::string default_node_id() {
    char host[256] = {0};
    if (::gethostname(host, sizeof host - 1) == 0 && host[0]) return host;
    return "node-" + rf::ulid().substr(0, 8);
}

// Pre-scan for --config so the file can be loaded before the command line is
// applied on top of it (precedence: defaults < file < argv).
std::string config_path_from_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) return argv[i + 1];
        if (rf::starts_with(arg, "--config=")) return arg.substr(9);
    }
    return std::string();
}

// D18: the router dispatches to the agent, and the agent forwards to its own
// loopback engine. This keeps the engine off the LAN entirely — only the agent
// is exposed, and only the agent authenticates — and it makes a simulated node
// indistinguishable from a real one to the router, which is what the Phase 1
// comparison depends on.
//
// The forward is byte for byte and chunk for chunk. The agent measures nothing
// and decides nothing here; buffering the stream would destroy the router's
// time-to-first-token measurement, which is the whole point of the exercise.
bool proxy_to_engine(const rf::http::Request& req, rf::http::Responder& res,
                     const std::string& host, uint16_t port, int timeout_ms) {
    rf::http::ClientRequest up;
    up.host = host;
    up.port = port;
    up.method = "POST";
    up.path = req.path;
    up.body = req.body;
    up.connect_timeout_ms = 5000;
    up.read_timeout_ms = timeout_ms;
    up.headers.set("Content-Type", req.headers.get("Content-Type", "application/json"));
    up.headers.set("Accept", req.headers.get("Accept", "*/*"));
    up.keep_going = [&res] { return res.alive(); };

    bool began = false;
    up.on_headers = [&](int status, const rf::http::Headers& headers) {
        rf::http::Headers out;
        out.set("Content-Type", headers.get("Content-Type", "application/json"));
        out.set("Cache-Control", "no-cache");
        if (!res.begin(status, out)) return false;
        began = true;
        return true;
    };
    up.on_chunk = [&](const char* data, size_t n) { return res.write(data, n); };

    rf::http::ClientResponse up_res;
    std::string err;
    const bool ok = rf::http::perform(up, up_res, &err);
    if (began) {
        res.end();
    } else if (!ok) {
        // Nothing reached the router yet, so this is still a clean failure it
        // can retry elsewhere (§6.4).
        res.send_error(502, "engine_unreachable",
                       "local engine at " + host + ":" + std::to_string(port) +
                           " did not answer: " + err);
    }
    return true;
}

bool authorised(const rf::http::Request& req, const std::string& token) {
    if (token.empty()) return true;
    const std::string header = req.headers.get("Authorization");
    return header == "Bearer " + token;
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

    const std::string node_id = cfg.get_str("node.id", default_node_id());
    const std::string bind = cfg.get_str("http.bind", "127.0.0.1");
    const uint16_t port = static_cast<uint16_t>(cfg.get_u32("http.port", 8971));
    const std::string token = cfg.get_str("http.token");
    const int poll_ms = static_cast<int>(cfg.get_u32("poll_ms", 1000));
    const int engine_timeout_ms = static_cast<int>(cfg.get_u32("engine.timeout_ms", 2000));
    // Separate from the probe timeout: a cold model load legitimately takes
    // minutes, while a /api/tags probe that hangs for 2 s is already broken.
    const int proxy_timeout_ms =
        static_cast<int>(cfg.get_u32("engine.proxy_timeout_ms", 600000));

    // ARCHITECTURE §10: LAN exposure is opt-in and authenticated, from the first
    // commit. Refusing to start is the only version of this rule that holds.
    if (bind != "127.0.0.1" && token.empty()) {
        std::fprintf(stderr,
                     "refusing to bind %s without --http.token: an unauthenticated "
                     "agent exposes its model inventory and accepts dispatch from "
                     "anyone on the network\n",
                     bind.c_str());
        return 2;
    }

    // --- simulated node ------------------------------------------------------
    std::unique_ptr<rf::SimNode> sim;
    if (cfg.has("simulate")) {
        const std::string path = cfg.get_str("simulate");
        std::string text, err;
        if (!rf::read_file(path, text)) {
            std::fprintf(stderr, "cannot read simulate profile %s\n", path.c_str());
            return 2;
        }
        rf::Json j;
        if (!rf::Json::parse(text, j, &err)) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
            return 2;
        }
        rf::SimProfile profile;
        if (!rf::SimProfile::from_json(j, profile, &err)) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
            return 2;
        }
        sim.reset(new rf::SimNode(std::move(profile)));
        RF_INFO("simulated node '%s': %s, %.1f GiB VRAM, %u slot(s), %zu model(s)",
                sim->profile().id.c_str(), sim->profile().gpu_name.c_str(),
                static_cast<double>(sim->profile().vram_total_bytes) / (1024.0 * 1024 * 1024),
                sim->profile().engine_slots, sim->profile().models.size());
    }

    // --- telemetry -----------------------------------------------------------
    std::unique_ptr<rf::ITelemetry> telemetry;
    if (!sim) {
        telemetry = rf::select_telemetry(cfg.get_str("telemetry"));
        if (!telemetry) return 2;  // an explicitly requested backend was missing
    }

    // --- engine probe --------------------------------------------------------
    std::unique_ptr<rf::IEngineProbe> engine;
    rf::EngineKind engine_kind = rf::EngineKind::Simulated;
    std::string engine_host = "127.0.0.1";
    uint16_t engine_port = 0;
    if (!sim) {
        const std::string kind_name = cfg.get_str("engine.kind", "ollama");
        engine_kind = rf::engine_kind_from_string(kind_name);
        const uint16_t default_port = engine_kind == rf::EngineKind::LMStudio ? 1234 : 11434;
        engine_port = default_port;
        const std::string endpoint =
            cfg.get_str("engine.endpoint", "127.0.0.1:" + std::to_string(default_port));
        if (!rf::http::parse_endpoint(endpoint, engine_host, engine_port, default_port)) {
            std::fprintf(stderr, "bad engine.endpoint '%s'\n", endpoint.c_str());
            return 2;
        }
        if (engine_kind == rf::EngineKind::Ollama) {
            engine = rf::make_ollama_probe(engine_host, engine_port, engine_timeout_ms);
        } else if (engine_kind == rf::EngineKind::LMStudio) {
            engine = rf::make_lmstudio_probe(engine_host, engine_port, engine_timeout_ms);
        } else {
            std::fprintf(stderr, "unknown engine.kind '%s' (ollama|lmstudio)\n",
                         kind_name.c_str());
            return 2;
        }
        RF_INFO("engine: %s at %s:%u", engine->name(), engine_host.c_str(), engine_port);
    }

    // engine_slots cannot be discovered: neither Ollama nor LM Studio reports
    // its parallelism over the API. It is configuration, and T_queue depends on
    // it, so a wrong value here shows up as a systematic queue-time error.
    const uint32_t engine_slots =
        sim ? sim->profile().engine_slots : std::max(1u, cfg.get_u32("engine.slots", 1));

    SharedState shared;

    // --- poll loop -----------------------------------------------------------
    std::thread poller([&] {
        float smoothed_util = 0.f;
        bool have_util = false;
        // Lives across polls: what it knows is the difference between two of
        // them (D38).
        rf::ResidencyLimit residency_limit;
        // Light smoothing, as §5 specifies: raw utilization is spiky enough to
        // flip a ranking between two samples of an otherwise identical cluster.
        constexpr float kUtilAlpha = 0.3f;

        while (!g_stop.load()) {
            rf::NodeState s;
            s.id = node_id;
            s.engine_slots = engine_slots;
            s.sampled_at_ms = rf::now_ms();

            if (sim) {
                s.gpu_name = sim->profile().gpu_name;
                s.telemetry_backend = "sim";
                s.telemetry_ok = true;
                s.vram_total_bytes = sim->profile().vram_total_bytes;
                s.vram_free_bytes = sim->vram_free_bytes();
                s.gpu_util = sim->gpu_util();
                s.temperature_c = static_cast<float>(sim->profile().temperature_c);
                if (sim->has_power_signal()) {
                    s.power_watts = sim->power_watts();
                    s.power_cap_watts = static_cast<float>(sim->profile().power_cap_watts);
                }
                s.engine = rf::EngineKind::Simulated;

                const rf::EngineReport r = sim->report();
                s.engine_healthy = r.healthy;
                s.residency_known = r.residency_known;
                s.models_on_disk = r.models_on_disk;
                s.model_disk_bytes = r.model_disk_bytes;
                s.models_resident = r.models_resident;
                s.inflight_reported = r.inflight;
                s.models_resident_limit = residency_limit.observe(
                    s.models_resident, s.residency_known, s.vram_free_bytes,
                    s.sampled_at_ms);
            } else {
                s.telemetry_backend = telemetry->name();
                std::vector<rf::GpuSample> samples;
                if (telemetry->sample(samples) && !samples.empty()) {
                    // Multi-GPU nodes are out of scope: one request runs on one
                    // node and one device. Device 0 is reported; the rest are
                    // named in the log so the omission is visible.
                    const rf::GpuSample& g = samples.front();
                    if (samples.size() > 1) {
                        static bool warned = false;
                        if (!warned) {
                            warned = true;
                            RF_WARN("node has %zu GPUs; reporting device 0 (%s) only",
                                    samples.size(), g.name.c_str());
                        }
                    }
                    s.gpu_name = g.name;
                    s.vram_total_bytes = g.vram_total_bytes;
                    s.vram_free_bytes = g.vram_free_bytes;
                    s.telemetry_ok = g.has_util || g.has_temperature || g.has_power;
                    if (g.has_util) {
                        smoothed_util = have_util
                                            ? kUtilAlpha * g.util + (1 - kUtilAlpha) * smoothed_util
                                            : g.util;
                        have_util = true;
                        s.gpu_util = smoothed_util;
                    }
                    if (g.has_temperature) s.temperature_c = g.temperature_c;
                    if (g.has_power) s.power_watts = g.power_watts;
                    if (g.has_power_cap) s.power_cap_watts = g.power_cap_watts;
                }

                s.engine = engine_kind;
                rf::EngineReport r;
                if (engine->poll(r)) {
                    s.engine_healthy = r.healthy;
                    s.residency_known = r.residency_known;
                    s.models_on_disk = std::move(r.models_on_disk);
                    s.model_disk_bytes = std::move(r.model_disk_bytes);
                    s.models_resident = std::move(r.models_resident);
                    s.inflight_reported = r.inflight;
                    s.models_resident_limit = residency_limit.observe(
                        s.models_resident, s.residency_known, s.vram_free_bytes,
                        s.sampled_at_ms);
                } else {
                    // A dead engine is a visible unhealthy node, not a missing
                    // one: the router must be able to say why it was rejected.
                    s.engine_healthy = false;
                }
            }

            shared.publish(std::move(s));

            for (int slept = 0; slept < poll_ms && !g_stop.load(); slept += 50)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        shared.wake_all();  // release any SSE waiter so the server can shut down
    });

    // --- http ---------------------------------------------------------------
    rf::http::Server server;
    g_server = &server;
    server.set_worker_threads(static_cast<int>(cfg.get_u32("http.threads", 24)));
    server.set_handler([&](const rf::http::Request& req, rf::http::Responder& res) {
        if (req.path == "/health") {
            res.send_json(200, "{\"ok\":true,\"node\":\"" + node_id + "\"}");
            return;
        }

        // In simulate mode the engine endpoints live here too, and they are
        // authenticated exactly like /state: a simulated node must not be a
        // hole in the same-shaped security model.
        if (!authorised(req, token)) {
            res.send_error(401, "unauthorized", "missing or bad bearer token");
            return;
        }

        if (sim && sim->handle(req, res)) return;

        // Engine passthrough for real nodes (D18).
        if (!sim && req.method == "POST" &&
            (req.path == "/v1/chat/completions" || req.path == "/api/chat" ||
             req.path == "/api/generate")) {
            proxy_to_engine(req, res, engine_host, engine_port, proxy_timeout_ms);
            return;
        }

        if (req.path == "/state") {
            res.send_json(200, shared.snapshot().to_json().dump());
            return;
        }

        if (req.path == "/events") {
            rf::http::Headers h;
            h.set("Content-Type", "text/event-stream");
            h.set("Cache-Control", "no-cache");
            h.set("X-Accel-Buffering", "no");
            if (!res.begin(200, h)) return;

            uint64_t seen = 0;
            const rf::NodeState first = shared.snapshot(&seen);
            if (!res.sse("state", first.to_json().dump())) return;

            while (res.alive() && !g_stop.load()) {
                const uint64_t now = shared.wait_for_change(seen, 5000);
                if (now == seen) {
                    // Idle: a comment frame keeps the connection warm and lets
                    // us notice a vanished peer without waiting for a change.
                    if (!res.write(": keepalive\n\n")) break;
                    continue;
                }
                seen = now;
                if (!res.sse("state", shared.snapshot().to_json().dump())) break;
            }
            res.end();
            return;
        }

        res.send_error(404, "not_found", "no route for " + req.path);
    });

    std::string err;
    if (!server.listen(bind, port, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        g_stop.store(true);
        poller.join();
        return 2;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    RF_INFO("routeflow-agent '%s' listening on %s:%u (%s)", node_id.c_str(),
            bind.c_str(), server.port(),
            token.empty() ? "no auth, loopback only" : "bearer auth");
    server.run();

    g_stop.store(true);
    shared.wake_all();
    poller.join();
    RF_INFO("routeflow-agent stopped");
    return 0;
}
