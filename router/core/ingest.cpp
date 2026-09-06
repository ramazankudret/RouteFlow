#include "router/core/ingest.h"

#include "common/json.h"
#include "common/util.h"

namespace rf {
namespace {

// Concatenates everything the model will have to prefill. Both the plain string
// form and the OpenAI content-parts array form.
std::string collect_prompt(const Json& body) {
    std::string text;
    const Json& messages = body["messages"];
    for (size_t i = 0; i < messages.size(); ++i) {
        const Json& m = messages.at(i);
        text += m["role"].as_str();
        text += ": ";
        const Json& content = m["content"];
        if (content.is_str()) {
            text += content.as_str();
        } else if (content.is_array()) {
            for (size_t k = 0; k < content.size(); ++k) text += content.at(k)["text"].as_str();
        }
        text += "\n";
    }
    if (text.empty()) text = body["prompt"].as_str();
    // Tool definitions are prefilled too and can dwarf the conversation.
    if (body.has("tools")) text += body["tools"].dump();
    if (body.has("system")) text += body["system"].as_str();
    return text;
}

uint32_t requested_max_tokens(const Json& body) {
    for (const char* key : {"max_tokens", "max_completion_tokens", "num_predict"}) {
        const uint32_t v = body[key].as_u32(0);
        if (v > 0) return v;
    }
    return body["options"]["num_predict"].as_u32(0);
}

uint32_t requested_num_ctx(const Json& body) {
    const uint32_t v = body["options"]["num_ctx"].as_u32(0);
    return v > 0 ? v : body["num_ctx"].as_u32(0);
}

}  // namespace

bool ingest(const http::Request& req, IngestResult& out, bool request_usage) {
    out = IngestResult();

    if (req.path == "/v1/chat/completions") {
        out.shape = ApiShape::OpenAIChat;
    } else if (req.path == "/api/chat") {
        out.shape = ApiShape::OllamaChat;
    } else if (req.path == "/api/generate") {
        out.shape = ApiShape::OllamaGenerate;
    } else {
        out.error_type = "not_found";
        out.error = "no route for " + req.path;
        return false;
    }
    out.upstream_path = req.path;

    Json body;
    std::string perr;
    if (!Json::parse(req.body, body, &perr)) {
        out.error_type = "invalid_request_error";
        out.error = "bad JSON: " + perr;
        return false;
    }

    RequestFeatures& f = out.features;
    f.model = body["model"].as_str();
    if (f.model.empty()) {
        out.error_type = "invalid_request_error";
        out.error = "request has no \"model\"";
        return false;
    }

    // The one rewrite (D37). On the OpenAI shape a streamed reply carries no
    // token counts unless the caller asks for them, and Ollama follows that
    // exactly. Without the counts `observe()` learns no decode rate, no prefill
    // rate and no output length, so against a real engine the learned cost
    // model silently degenerates into the static one -- which is what a run on
    // real hardware showed.
    //
    // Only the OpenAI shape, only when streaming, and never over a caller who
    // set `stream_options` themselves. The Ollama-native shapes already report
    // `eval_count` and need nothing.
    //
    // The cost is one extra chunk to the client, carrying usage and no choices.
    // That is the documented OpenAI shape, but this client did not ask for it,
    // so it is a real change to what they receive and `dispatch.request_usage`
    // turns it off.
    if (out.shape == ApiShape::OpenAIChat && request_usage &&
        body.has("stream") && body["stream"].as_bool() && !body.has("stream_options")) {
        Json rewritten = body;
        Json options = Json::object();
        options["include_usage"] = Json(true);
        rewritten["stream_options"] = std::move(options);
        out.upstream_body = rewritten.dump();
    }

    // Ollama streams by default; OpenAI does not.
    const bool ollama = out.shape != ApiShape::OpenAIChat;
    f.stream = body.has("stream") ? body["stream"].as_bool() : ollama;
    f.num_ctx = requested_num_ctx(body);

    // D15: estimated, never tokenized. The trace records this alongside the
    // engine-reported actual so the error is measured rather than argued about
    // — and the first real measurement said the naive count was 63% low (15
    // estimated against 41 reported). The gap is structural, not noise: every
    // chat template wraps each message in role markers, and most models inject
    // a default system prompt when the caller supplies none. Both are charged
    // here. These constants are calibrated, not derived; the trace keeps
    // measuring the residual, and Phase 2 can learn it per model.
    constexpr uint32_t kTokensPerMessage = 8;
    constexpr uint32_t kImplicitSystemPromptTokens = 16;

    uint32_t message_count = static_cast<uint32_t>(body["messages"].size());
    bool has_system = body.has("system");
    for (size_t i = 0; i < body["messages"].size(); ++i)
        if (body["messages"].at(i)["role"].as_str() == "system") has_system = true;
    if (message_count == 0) message_count = 1;  // /api/generate: a bare prompt

    f.prompt_tokens = estimate_tokens(collect_prompt(body)) +
                      message_count * kTokensPerMessage +
                      (has_system ? 0u : kImplicitSystemPromptTokens);

    // §8 says role_hint is optional and must never be required.
    f.role_hint = req.headers.get("X-RouteFlow-Role");
    if (f.role_hint.empty()) f.role_hint = body["routeflow"]["role"].as_str();

    // Output length is the least reliable input to the largest term (D8). When
    // the caller caps it we have a real upper bound, which beats the cost
    // model's blind prior — but it *is* an upper bound: models stop early, so
    // the sigma stays wide rather than pretending a cap is a prediction. When
    // there is no cap we leave both at zero and let the cost model's own prior
    // answer.
    const uint32_t cap = requested_max_tokens(body);
    if (cap > 0) {
        f.predicted_output_tokens = cap;
        f.predicted_output_sigma = static_cast<float>(cap) * 0.35f;
    }

    out.ok = true;
    return true;
}

}  // namespace rf
