// RouteFlow — request ingest (ARCHITECTURE §4.2 step 1).
//
// Accepts the OpenAI-compatible shape and the Ollama-native shape so existing
// tools work unchanged. Ingest only reads enough to score the request, and the
// body reaches the winning node unchanged -- with one exception, named in
// `upstream_body` below and argued in D37.
#pragma once

#include <string>

#include "common/http.h"
#include "common/types.h"

namespace rf {

enum class ApiShape { OpenAIChat, OllamaChat, OllamaGenerate };

struct IngestResult {
    bool ok = false;
    std::string error;      // client-facing, when !ok
    std::string error_type;

    ApiShape shape = ApiShape::OpenAIChat;
    std::string upstream_path;  // same path the client used
    // Empty means "forward the client's bytes". Non-empty is the one rewrite
    // this router performs: asking an OpenAI-shaped stream for its usage
    // totals, without which the cost model has nothing to learn from (D37).
    std::string upstream_body;
    RequestFeatures features;
};

// `path` is the router-side route; the upstream path is identical, because a
// node speaks the same API the client does.
// `request_usage` enables the one rewrite described above.
bool ingest(const http::Request& req, IngestResult& out, bool request_usage = true);

}  // namespace rf
