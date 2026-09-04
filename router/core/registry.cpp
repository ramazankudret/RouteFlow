#include "router/core/registry.h"

#include <chrono>
#include <mutex>
#include <set>

#include "common/http.h"
#include "common/util.h"

namespace rf {

struct NodeRegistry::Slot {
    NodeConfig config;
    mutable std::mutex mu;
    NodeState state;
    bool ever_seen = false;
    int64_t last_ok_ms = 0;
    uint64_t poll_failures = 0;
    bool logged_down = false;
};

NodeRegistry::NodeRegistry() = default;

NodeRegistry::~NodeRegistry() { stop(); }

bool NodeRegistry::load(const Json& nodes, std::string* err) {
    if (!nodes.is_array() || nodes.size() == 0) {
        if (err) *err = "nodes must be a non-empty array";
        return false;
    }

    std::set<std::string> seen_ids;
    std::vector<NodeConfig> parsed;

    for (size_t i = 0; i < nodes.size(); ++i) {
        const Json& n = nodes.at(i);
        NodeConfig c;
        c.id = n["id"].as_str();
        if (c.id.empty()) {
            if (err) *err = "node[" + std::to_string(i) + "] has no id";
            return false;
        }
        // Two nodes sharing an id would merge in the ledger and in the trace,
        // producing statistics that quietly describe a machine that does not
        // exist.
        if (!seen_ids.insert(c.id).second) {
            if (err) *err = "duplicate node id '" + c.id + "'";
            return false;
        }

        const std::string endpoint = n["endpoint"].as_str();
        if (endpoint.empty()) {
            if (err) *err = "node '" + c.id + "' has no endpoint";
            return false;
        }
        if (!http::parse_endpoint(endpoint, c.host, c.port, 8971)) {
            if (err) *err = "node '" + c.id + "': bad endpoint '" + endpoint + "'";
            return false;
        }

        if (n.has("engine_endpoint")) {
            const std::string ee = n["engine_endpoint"].as_str();
            if (!http::parse_endpoint(ee, c.engine_host, c.engine_port, 11434)) {
                if (err) *err = "node '" + c.id + "': bad engine_endpoint '" + ee + "'";
                return false;
            }
            c.dispatch_direct_to_engine = true;
        }
        c.excluded = n["excluded"].as_bool(false);
        parsed.push_back(std::move(c));
    }

    configs_ = std::move(parsed);
    slots_.clear();
    for (const auto& c : configs_) {
        std::unique_ptr<Slot> slot(new Slot());
        slot->config = c;
        slot->state.id = c.id;
        slot->state.engine_healthy = false;  // nothing known until first poll
        slots_.push_back(std::move(slot));
    }
    return true;
}

const NodeConfig* NodeRegistry::config_for(const std::string& node_id) const {
    for (const auto& c : configs_)
        if (c.id == node_id) return &c;
    return nullptr;
}

std::vector<std::string> NodeRegistry::excluded_ids() const {
    std::vector<std::string> out;
    for (const auto& c : configs_)
        if (c.excluded) out.push_back(c.id);
    return out;
}

void NodeRegistry::start(const std::string& token, int poll_ms, int timeout_ms) {
    if (running_.exchange(true)) return;

    for (auto& slot_ptr : slots_) {
        Slot* slot = slot_ptr.get();
        pollers_.emplace_back([this, slot, token, poll_ms, timeout_ms] {
            while (running_.load()) {
                http::ClientRequest req;
                req.host = slot->config.host;
                req.port = slot->config.port;
                req.path = "/state";
                req.connect_timeout_ms = timeout_ms;
                req.read_timeout_ms = timeout_ms;
                req.headers.set("Accept", "application/json");
                if (!token.empty()) req.headers.set("Authorization", "Bearer " + token);

                http::ClientResponse res;
                std::string err;
                bool ok = false;
                NodeState fresh;

                if (http::perform(req, res, &err) && res.status == 200) {
                    Json j;
                    std::string perr;
                    if (Json::parse(res.body, j, &perr) &&
                        NodeState::from_json(j, fresh)) {
                        // The agent reports its own id; the registry's id is
                        // authoritative. A mismatch means the config points at
                        // the wrong machine — worth saying out loud, because
                        // every trace record would otherwise be mislabelled.
                        if (fresh.id != slot->config.id) {
                            RF_WARN("node '%s' at %s:%u reports id '%s'; using "
                                    "the configured id",
                                    slot->config.id.c_str(), slot->config.host.c_str(),
                                    slot->config.port, fresh.id.c_str());
                            fresh.id = slot->config.id;
                        }
                        ok = true;
                    } else {
                        err = "bad /state payload: " + perr;
                    }
                } else if (res.status != 0 && res.status != 200) {
                    err = "status " + std::to_string(res.status);
                }

                {
                    std::lock_guard<std::mutex> lock(slot->mu);
                    if (ok) {
                        // The agent stamps sampled_at_ms with its own clock.
                        // Staleness is judged against ours (§6.1), so restamp:
                        // a node with a skewed clock must not look permanently
                        // stale or permanently fresh.
                        fresh.sampled_at_ms = now_ms();
                        slot->state = std::move(fresh);
                        slot->ever_seen = true;
                        slot->last_ok_ms = now_ms();
                        if (slot->logged_down) {
                            slot->logged_down = false;
                            RF_INFO("node '%s' is back", slot->config.id.c_str());
                        }
                    } else {
                        ++slot->poll_failures;
                        // Keep the last known state but clear health, so the
                        // node stays a visible, explainable rejection rather
                        // than disappearing from the decision breakdown.
                        slot->state.engine_healthy = false;
                        if (!slot->logged_down) {
                            slot->logged_down = true;
                            RF_WARN("node '%s' unreachable at %s:%u: %s",
                                    slot->config.id.c_str(), slot->config.host.c_str(),
                                    slot->config.port, err.c_str());
                        }
                    }
                }

                for (int slept = 0; slept < poll_ms && running_.load(); slept += 50)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }
}

void NodeRegistry::stop() {
    if (!running_.exchange(false)) return;
    for (auto& t : pollers_)
        if (t.joinable()) t.join();
    pollers_.clear();
}

std::vector<NodeState> NodeRegistry::snapshot() const {
    std::vector<NodeState> out;
    out.reserve(slots_.size());
    for (const auto& slot : slots_) {
        std::lock_guard<std::mutex> lock(slot->mu);
        out.push_back(slot->state);
    }
    return out;
}

Json NodeRegistry::to_json() const {
    Json arr = Json::array();
    for (const auto& slot : slots_) {
        std::lock_guard<std::mutex> lock(slot->mu);
        Json j = slot->state.to_json();
        j["endpoint"] = Json(slot->config.host + ":" + std::to_string(slot->config.port));
        j["dispatch_endpoint"] =
            Json(slot->config.dispatch_host() + ":" +
                 std::to_string(slot->config.dispatch_port()));
        j["excluded"] = Json(slot->config.excluded);
        j["ever_seen"] = Json(slot->ever_seen);
        j["poll_failures"] = Json(slot->poll_failures);
        arr.push_back(std::move(j));
    }
    return arr;
}

}  // namespace rf
