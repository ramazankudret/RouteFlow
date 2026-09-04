// Ollama probe.
//
//   GET /api/tags  -> models on disk   {"models":[{"name","size","digest",...}]}
//   GET /api/ps    -> models in memory {"models":[{"name","size","size_vram",...}]}
//
// /api/ps is what makes warmth observable: `size_vram` is the actual resident
// footprint, which is also the first real measurement the cost model gets for
// D2 (footprint learning).

#include "agent/engine/engine_probe.h"

#include "common/http.h"
#include "common/json.h"
#include "common/util.h"

namespace rf {
namespace {

class OllamaProbe : public IEngineProbe {
public:
    OllamaProbe(std::string host, uint16_t port, int timeout_ms)
        : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms) {}

    EngineKind kind() const override { return EngineKind::Ollama; }
    const char* name() const override { return "ollama"; }

    bool poll(EngineReport& out) override {
        out = EngineReport();

        Json tags;
        if (!get_json("/api/tags", tags)) return false;  // engine down

        for (size_t i = 0; i < tags["models"].size(); ++i) {
            const Json& m = tags["models"].at(i);
            const std::string model = m["name"].as_str();
            if (model.empty()) continue;
            out.models_on_disk.push_back(model);
            const uint64_t bytes = m["size"].as_u64();
            if (bytes > 0) out.model_disk_bytes.emplace_back(model, bytes);
        }

        // A healthy engine with an empty /api/ps simply has nothing loaded;
        // a failing /api/ps is a different thing and must not read as "cold".
        Json ps;
        if (!get_json("/api/ps", ps)) {
            out.residency_known = false;
            out.healthy = true;
            RF_WARN("ollama: /api/ps unavailable; residency unknown on this node");
            return true;
        }

        for (size_t i = 0; i < ps["models"].size(); ++i) {
            const Json& m = ps["models"].at(i);
            ResidentModel r;
            r.name = m["name"].as_str();
            if (r.name.empty()) continue;
            // size_vram is the resident part; on partial CPU offload it is
            // smaller than size, and it is the number admission cares about.
            r.vram_bytes = m["size_vram"].as_u64();
            if (r.vram_bytes == 0) r.vram_bytes = m["size"].as_u64();
            // Ollama does not report when a model was last used, only when it
            // expires. The router owns last_used because it knows what it
            // dispatched; inventing it here would be a guess in a field the
            // eviction policy reads as fact.
            r.last_used_ms = 0;
            out.models_resident.push_back(std::move(r));
        }

        out.healthy = true;
        return true;
    }

private:
    bool get_json(const char* path, Json& out) {
        http::ClientRequest req;
        req.host = host_;
        req.port = port_;
        req.path = path;
        req.connect_timeout_ms = timeout_ms_;
        req.read_timeout_ms = timeout_ms_;
        req.headers.set("Accept", "application/json");

        http::ClientResponse res;
        std::string err;
        if (!http::perform(req, res, &err)) {
            RF_DEBUG("ollama %s: %s", path, err.c_str());
            return false;
        }
        if (res.status != 200) {
            RF_DEBUG("ollama %s: status %d", path, res.status);
            return false;
        }
        std::string perr;
        if (!Json::parse(res.body, out, &perr)) {
            RF_WARN("ollama %s: bad json: %s", path, perr.c_str());
            return false;
        }
        return true;
    }

    std::string host_;
    uint16_t port_;
    int timeout_ms_;
};

}  // namespace

std::unique_ptr<IEngineProbe> make_ollama_probe(const std::string& host, uint16_t port,
                                                int timeout_ms) {
    return std::unique_ptr<IEngineProbe>(new OllamaProbe(host, port, timeout_ms));
}

}  // namespace rf
