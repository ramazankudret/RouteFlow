// RouteFlow — node registry (ARCHITECTURE §12: static config, no discovery).
//
// Node addresses come from `nodes.json` and nowhere else — never from a
// response body (§10). A compromised or spoofed agent can lie about its own
// telemetry, which costs us a bad routing decision; it must never be able to
// redirect dispatch at a machine we did not configure.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/json.h"
#include "common/types.h"

namespace rf {

struct NodeConfig {
    std::string id;
    // The agent's address. Dispatch goes here too (D18): the engine stays on
    // the node's loopback, and the agent is the one authenticated front door.
    std::string host;
    uint16_t port = 8971;
    // Optional escape hatch for an engine on a machine that cannot run an
    // agent. Off by default; the trace records which path a job took.
    bool dispatch_direct_to_engine = false;
    std::string engine_host;
    uint16_t engine_port = 0;
    // Operator exclusion (§6.1). Kept here rather than in NodeState because it
    // is our policy about the node, not something the node reports.
    bool excluded = false;

    std::string dispatch_host() const {
        return dispatch_direct_to_engine ? engine_host : host;
    }
    uint16_t dispatch_port() const {
        return dispatch_direct_to_engine ? engine_port : port;
    }
};

class NodeRegistry {
public:
    // Both out of line: Slot is incomplete here, and an inline constructor
    // would need it complete to emit the vector's cleanup path.
    NodeRegistry();
    ~NodeRegistry();
    NodeRegistry(const NodeRegistry&) = delete;
    NodeRegistry& operator=(const NodeRegistry&) = delete;

    // `nodes` is the array from nodes.json. Rejects duplicate ids: two nodes
    // sharing an id would silently merge in the ledger and in the trace.
    bool load(const Json& nodes, std::string* err);

    const std::vector<NodeConfig>& configs() const { return configs_; }
    const NodeConfig* config_for(const std::string& node_id) const;
    std::vector<std::string> excluded_ids() const;

    // Starts one polling thread per node. Each polls GET /state and publishes
    // into the shared snapshot. A node that fails to answer keeps its last
    // state with engine_healthy cleared, so it stays a *visible* rejected
    // candidate rather than vanishing from the decision breakdown (§10).
    void start(const std::string& token, int poll_ms, int timeout_ms);
    void stop();

    // Registry order, which is config order — stable across runs, which the
    // RoundRobin baseline depends on for reproducibility.
    std::vector<NodeState> snapshot() const;

    Json to_json() const;

private:
    struct Slot;

    std::vector<NodeConfig> configs_;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::vector<std::thread> pollers_;
    std::atomic<bool> running_{false};
};

}  // namespace rf
