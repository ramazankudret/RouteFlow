// RouteFlow — dispatch (ARCHITECTURE §4.2 steps 5-7, §6.4, D9, D18).
//
// Proxies the request to the winning node byte for byte, measures it, and
// appends exactly one trace record per attempt. A retry that did not appear in
// the trace would corrupt every latency statistic the project produces, so
// retries are recorded as their own records linked by `retry_of`.
#pragma once

#include <string>

#include "common/http.h"
#include "router/core/registry.h"
#include "router/core/router_state.h"
#include "router/core/trace_writer.h"

namespace rf {

class Dispatcher {
public:
    Dispatcher(RouterState& state, NodeRegistry& registry, TraceWriter& trace,
               std::string node_token, int request_timeout_ms);

    // Handles one client request end to end: ingest, score, reserve, proxy,
    // record. Writes the client response itself.
    void handle(const http::Request& req, http::Responder& res);

private:
    RouterState& state_;
    NodeRegistry& registry_;
    TraceWriter& trace_;
    std::string node_token_;
    int request_timeout_ms_;
};

}  // namespace rf
