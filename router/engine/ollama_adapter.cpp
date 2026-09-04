// Ollama engine adapter.
//
// Ollama has no dedicated load or unload endpoint. Both are expressed through
// /api/generate with an empty prompt and a keep_alive: a positive duration
// loads the model and holds it, and zero drops it immediately. That is the
// documented mechanism, not a trick — but it does mean a placement action goes
// down the same path as inference and occupies a slot while it runs, which is
// why the placement manager treats a preload as real work rather than a hint.

#include "router/engine/engine_adapter.h"

#include "common/http.h"
#include "common/json.h"
#include "common/util.h"

namespace rf {
namespace {

class OllamaAdapter : public IEngineAdapter {
public:
    const char* name() const override { return "ollama"; }

    bool preload(const EngineTarget& target, const std::string& model,
                 int timeout_ms, std::string* err) override {
        // keep_alive is a hint to the engine, not a lease we own: the manager
        // re-asserts placement on its own schedule, so a long value here only
        // means "do not drop this the moment I stop looking".
        return call(target, model, "10m", timeout_ms, err);
    }

    bool evict(const EngineTarget& target, const std::string& model,
               int timeout_ms, std::string* err) override {
        return call(target, model, "0", timeout_ms, err);
    }

private:
    bool call(const EngineTarget& target, const std::string& model,
              const std::string& keep_alive, int timeout_ms, std::string* err) {
        Json body = Json::object();
        body["model"] = Json(model);
        body["prompt"] = Json("");        // load or unload only; generate nothing
        body["stream"] = Json(false);
        body["keep_alive"] = Json(keep_alive);

        http::ClientRequest req;
        req.host = target.host;
        req.port = target.port;
        req.method = "POST";
        req.path = "/api/generate";
        req.body = body.dump();
        req.connect_timeout_ms = 5000;
        req.read_timeout_ms = timeout_ms;
        req.headers.set("Content-Type", "application/json");
        if (!target.token.empty())
            req.headers.set("Authorization", "Bearer " + target.token);

        http::ClientResponse res;
        std::string transport_err;
        if (!http::perform(req, res, &transport_err)) {
            if (err) *err = transport_err.empty() ? "unreachable" : transport_err;
            return false;
        }
        if (res.status != 200) {
            if (err)
                *err = "status " + std::to_string(res.status) + ": " +
                       res.body.substr(0, 200);
            return false;
        }
        return true;
    }
};

}  // namespace

std::unique_ptr<IEngineAdapter> make_ollama_adapter() {
    return std::unique_ptr<IEngineAdapter>(new OllamaAdapter());
}

std::unique_ptr<IEngineAdapter> make_engine_adapter(EngineKind kind) {
    switch (kind) {
        case EngineKind::Ollama:
        // The simulated node speaks Ollama's API precisely so that placement is
        // exercised in the bench the same way it runs on hardware (D16).
        case EngineKind::Simulated:
            return make_ollama_adapter();
        case EngineKind::LMStudio:
            // LM Studio's load/unload is a different API and its older builds
            // do not report residency at all (D3). A node RouteFlow cannot
            // place on still routes; it just manages its own VRAM.
            return nullptr;
        case EngineKind::None:
            return nullptr;
    }
    return nullptr;
}

}  // namespace rf
