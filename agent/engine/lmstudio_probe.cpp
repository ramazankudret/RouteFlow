// LM Studio probe.
//
//   GET /api/v0/models -> per-model {"id","state":"loaded"|"not-loaded",...}
//   GET /v1/models     -> OpenAI-compatible list, no residency information
//
// The v0 endpoint is what makes this engine usable for warmth-aware routing.
// When only the OpenAI-compatible list is available we report
// residency_known = false rather than guessing that everything is cold —
// guessing would fabricate exactly the quantity the project sets out to
// measure (ARCHITECTURE §10, D3's "never silently zero" rule).

#include "agent/engine/engine_probe.h"

#include "common/http.h"
#include "common/json.h"
#include "common/util.h"

namespace rf {
namespace {

class LMStudioProbe : public IEngineProbe {
public:
    LMStudioProbe(std::string host, uint16_t port, int timeout_ms)
        : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms) {}

    EngineKind kind() const override { return EngineKind::LMStudio; }
    const char* name() const override { return "lmstudio"; }

    bool poll(EngineReport& out) override {
        out = EngineReport();

        Json v0;
        if (get_json("/api/v0/models", v0)) {
            for (size_t i = 0; i < v0["data"].size(); ++i) {
                const Json& m = v0["data"].at(i);
                const std::string id = m["id"].as_str();
                if (id.empty()) continue;
                out.models_on_disk.push_back(id);
                const uint64_t bytes = m["size_bytes"].as_u64();
                if (bytes > 0) out.model_disk_bytes.emplace_back(id, bytes);
                if (m["state"].as_str() == "loaded") {
                    ResidentModel r;
                    r.name = id;
                    // LM Studio reports no resident byte count. Leave it at 0;
                    // the cost model's footprint estimate fills the gap and
                    // corrects itself from the observed VRAM delta (D2).
                    r.vram_bytes = 0;
                    r.last_used_ms = 0;
                    out.models_resident.push_back(std::move(r));
                }
            }
            out.healthy = true;
            return true;
        }

        Json v1;
        if (!get_json("/v1/models", v1)) return false;  // engine down

        for (size_t i = 0; i < v1["data"].size(); ++i) {
            const std::string id = v1["data"].at(i)["id"].as_str();
            if (!id.empty()) out.models_on_disk.push_back(id);
        }
        out.residency_known = false;
        out.healthy = true;
        if (!warned_) {
            warned_ = true;
            RF_WARN("lmstudio: /api/v0/models unavailable; residency unknown on this "
                    "node, so T_load will be omitted from its score");
        }
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
            RF_DEBUG("lmstudio %s: %s", path, err.c_str());
            return false;
        }
        if (res.status != 200) return false;
        std::string perr;
        if (!Json::parse(res.body, out, &perr)) {
            RF_WARN("lmstudio %s: bad json: %s", path, perr.c_str());
            return false;
        }
        return true;
    }

    std::string host_;
    uint16_t port_;
    int timeout_ms_;
    bool warned_ = false;
};

}  // namespace

std::unique_ptr<IEngineProbe> make_lmstudio_probe(const std::string& host, uint16_t port,
                                                  int timeout_ms) {
    return std::unique_ptr<IEngineProbe>(new LMStudioProbe(host, port, timeout_ms));
}

}  // namespace rf
