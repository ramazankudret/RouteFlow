// RouteFlow — request ingest (ARCHITECTURE §4.2 step 1).
//
// Accepts the OpenAI-compatible shape and the Ollama-native shape so existing
// tools work unchanged. The body is NOT rewritten: it is proxied to the winning
// node byte for byte. Ingest only reads enough to score the request.
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
    RequestFeatures features;
};

// `path` is the router-side route; the upstream path is identical, because a
// node speaks the same API the client does.
bool ingest(const http::Request& req, IngestResult& out);

}  // namespace rf
