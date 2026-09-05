#include "agent/sim/sim_node.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>

#include "common/util.h"

namespace rf {
namespace {

void sleep_ms(double ms) {
    if (ms <= 0) return;
    std::this_thread::sleep_for(
        std::chrono::microseconds(static_cast<int64_t>(ms * 1000.0)));
}

// Extracts the prompt text from either request shape, so the simulated node
// charges for prefill the same way a real one would.
std::string extract_prompt(const Json& body) {
    std::string text;
    const Json& messages = body["messages"];
    for (size_t i = 0; i < messages.size(); ++i) {
        const Json& content = messages.at(i)["content"];
        if (content.is_str()) {
            text += content.as_str();
        } else if (content.is_array()) {  // OpenAI content-parts form
            for (size_t k = 0; k < content.size(); ++k)
                text += content.at(k)["text"].as_str();
        }
        text += "\n";
    }
    if (text.empty()) text = body["prompt"].as_str();
    return text;
}

uint32_t requested_output_tokens(const Json& body) {
    for (const char* key : {"max_tokens", "max_completion_tokens", "num_predict"}) {
        const uint32_t v = body[key].as_u32(0);
        if (v > 0) return v;
    }
    return body["options"]["num_predict"].as_u32(0);
}

// A caller that states no cap gets a reply drawn from the model's own length
// distribution. That is the ordinary case for agent traffic, and it is the case
// the router has to *predict* rather than be told (D29).
//
// The draw is a hash of (node seed, model, prompt) rather than a step of a
// shared RNG: sub-agent calls arrive concurrently, so a shared stream would
// hand out lengths in thread-completion order and the scenario would stop
// replaying identically (D12). Hashing the request makes the length a property
// of the request, which is also what it is in reality.
uint32_t drawn_output_tokens(uint64_t seed, const std::string& model,
                             const std::string& prompt, uint32_t lo, uint32_t hi) {
    uint64_t h = 1469598103934665603ULL ^ seed;
    for (const std::string* part : {&model, &prompt}) {
        for (unsigned char c : *part) {
            h ^= c;
            h *= 1099511628211ULL;
        }
    }
    // splitmix64 finalizer. FNV-1a leaves structure in the low bits and the
    // modulo below takes exactly those.
    h += 0x9e3779b97f4a7c15ULL;
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return lo + static_cast<uint32_t>(h % (static_cast<uint64_t>(hi - lo) + 1));
}

}  // namespace

// --- profile ----------------------------------------------------------------

bool SimProfile::from_json(const Json& j, SimProfile& out, std::string* err) {
    if (!j.is_object()) {
        if (err) *err = "profile must be a JSON object";
        return false;
    }
    out = SimProfile();
    out.id = j["id"].as_str();
    if (out.id.empty()) {
        if (err) *err = "profile needs an \"id\"";
        return false;
    }
    out.gpu_name = j["gpu_name"].as_str(out.id + " (simulated)");
    out.vram_total_bytes = j["vram_total_bytes"].as_u64(out.vram_total_bytes);
    out.engine_slots = std::max(1u, j["engine_slots"].as_u32(1));
    out.load_bandwidth_bytes_per_ms =
        j["load_bandwidth_bytes_per_ms"].as_num(out.load_bandwidth_bytes_per_ms);
    out.contention_alpha = j["contention_alpha"].as_num(out.contention_alpha);
    out.jitter = j["jitter"].as_num(out.jitter);
    out.seed = j["seed"].as_u64(1);
    out.idle_power_watts = j["idle_power_watts"].as_num(0);
    out.busy_power_watts = j["busy_power_watts"].as_num(0);
    out.power_cap_watts = j["power_cap_watts"].as_num(0);
    out.temperature_c = j["temperature_c"].as_num(0);

    const Json& models = j["models"];
    for (size_t i = 0; i < models.size(); ++i) {
        const Json& m = models.at(i);
        SimModelProfile p;
        p.name = m["name"].as_str();
        if (p.name.empty()) {
            if (err) *err = "every model needs a \"name\"";
            return false;
        }
        p.disk_bytes = m["disk_bytes"].as_u64(0);
        p.footprint_bytes = m["footprint_bytes"].as_u64(p.disk_bytes);
        p.prefill_tokens_per_ms = m["prefill_tokens_per_ms"].as_num(1.0);
        p.decode_tokens_per_ms = m["decode_tokens_per_ms"].as_num(0.02);
        p.output_tokens_min = m["output_tokens_min"].as_u32(0);
        p.output_tokens_max = m["output_tokens_max"].as_u32(0);
        if (p.footprint_bytes == 0 || p.prefill_tokens_per_ms <= 0 ||
            p.decode_tokens_per_ms <= 0) {
            if (err) *err = "model " + p.name + " has a non-positive rate or footprint";
            return false;
        }
        if (p.output_tokens_max < p.output_tokens_min) {
            if (err) *err = "model " + p.name + " has output_tokens_max below min";
            return false;
        }
        out.models.push_back(std::move(p));
    }
    if (out.models.empty()) {
        if (err) *err = "profile needs at least one model";
        return false;
    }
    for (size_t i = 0; i < j["resident_at_start"].size(); ++i)
        out.resident_at_start.push_back(j["resident_at_start"].at(i).as_str());
    return true;
}

// --- node -------------------------------------------------------------------

struct SimNode::Impl {
    SimProfile profile;

    mutable std::mutex mu;
    std::vector<ResidentModel> resident;  // insertion order == load order
    uint64_t vram_used = 0;
    uint32_t inflight = 0;
    uint32_t decoding = 0;
    std::mt19937_64 rng;

    // Engines serialise beyond their parallel slots; the sim must too, or
    // T_queue would have nothing to predict.
    std::condition_variable slot_cv;
    uint32_t slots_in_use = 0;

    explicit Impl(SimProfile p) : profile(std::move(p)), rng(profile.seed) {
        for (const auto& name : profile.resident_at_start) {
            const SimModelProfile* m = find(name);
            if (!m) {
                RF_WARN("sim %s: resident_at_start names unknown model '%s'",
                        profile.id.c_str(), name.c_str());
                continue;
            }
            if (vram_used + m->footprint_bytes > profile.vram_total_bytes) {
                RF_WARN("sim %s: '%s' does not fit at start; skipped",
                        profile.id.c_str(), name.c_str());
                continue;
            }
            resident.push_back({name, m->footprint_bytes, now_ms()});
            vram_used += m->footprint_bytes;
        }
    }

    const SimModelProfile* find(const std::string& name) const {
        for (const auto& m : profile.models)
            if (m.name == name) return &m;
        return nullptr;
    }

    // Multiplicative jitter around a modelled duration. Deterministic given the
    // profile seed, so a scenario replays identically.
    double jittered(double ms) {
        if (profile.jitter <= 0) return ms;
        std::normal_distribution<double> dist(1.0, profile.jitter);
        return std::max(0.0, ms * dist(rng));
    }

    bool is_resident(const std::string& name) const {
        for (const auto& r : resident)
            if (r.name == name) return true;
        return false;
    }

    // Evicts least-recently-used models until `need` bytes fit. Returns the
    // names evicted so the trace can show what this request cost the next one.
    std::vector<std::string> make_room(uint64_t need) {
        std::vector<std::string> evicted;
        while (vram_used + need > profile.vram_total_bytes && !resident.empty()) {
            size_t victim = 0;
            for (size_t i = 1; i < resident.size(); ++i)
                if (resident[i].last_used_ms < resident[victim].last_used_ms) victim = i;
            vram_used -= resident[victim].vram_bytes;
            evicted.push_back(resident[victim].name);
            resident.erase(resident.begin() + static_cast<long>(victim));
        }
        return evicted;
    }
};

SimNode::SimNode(SimProfile profile) : impl_(new Impl(std::move(profile))) {}
SimNode::~SimNode() = default;

const SimProfile& SimNode::profile() const { return impl_->profile; }

EngineReport SimNode::report() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    EngineReport r;
    r.healthy = true;
    r.residency_known = true;
    r.inflight = impl_->inflight;
    for (const auto& m : impl_->profile.models) {
        r.models_on_disk.push_back(m.name);
        if (m.disk_bytes > 0) r.model_disk_bytes.emplace_back(m.name, m.disk_bytes);
    }
    r.models_resident = impl_->resident;
    return r;
}

uint64_t SimNode::vram_free_bytes() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->profile.vram_total_bytes - impl_->vram_used;
}

uint32_t SimNode::inflight() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->inflight;
}

bool SimNode::has_power_signal() const { return impl_->profile.busy_power_watts > 0; }

float SimNode::power_watts() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    const auto& p = impl_->profile;
    return static_cast<float>(impl_->inflight > 0 ? p.busy_power_watts : p.idle_power_watts);
}

float SimNode::gpu_util() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    const uint32_t slots = impl_->profile.engine_slots;
    const float busy = static_cast<float>(std::min(impl_->inflight, slots));
    return slots > 0 ? busy / static_cast<float>(slots) : 0.f;
}

// A load costs what a load costs: the same modelled seconds an inference would
// have paid, and the same slot. Making placement free in simulation would hide
// the exact risk Phase 3 has to measure — that preloading steals capacity from
// the requests it was meant to help.
void SimNode::handle_placement(const std::string& model, const SimModelProfile& mp,
                               bool unload, http::Responder& res) {
    double load_ms = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        const bool resident = impl_->is_resident(model);
        if (unload) {
            if (resident) {
                for (size_t i = 0; i < impl_->resident.size(); ++i) {
                    if (impl_->resident[i].name != model) continue;
                    impl_->vram_used -= impl_->resident[i].vram_bytes;
                    impl_->resident.erase(impl_->resident.begin() +
                                          static_cast<long>(i));
                    break;
                }
            }
        } else if (!resident) {
            impl_->make_room(mp.footprint_bytes);
            if (impl_->vram_used + mp.footprint_bytes >
                impl_->profile.vram_total_bytes) {
                res.send_error(507, "insufficient_vram",
                               "model does not fit even with everything evicted");
                return;
            }
            load_ms = impl_->jittered(static_cast<double>(mp.footprint_bytes) /
                                      impl_->profile.load_bandwidth_bytes_per_ms);
            impl_->resident.push_back({model, mp.footprint_bytes, now_ms()});
            impl_->vram_used += mp.footprint_bytes;
        }
    }
    sleep_ms(load_ms);

    Json out = Json::object();
    out["model"] = Json(model);
    out["created_at"] = Json(iso8601(now_ms()));
    out["response"] = Json("");
    out["done"] = Json(true);
    out["done_reason"] = Json(unload ? "unload" : "load");
    out["load_duration"] = Json(load_ms * 1e6);  // Ollama reports nanoseconds
    res.send_json(200, out.dump());
}

bool SimNode::handle(const http::Request& req, http::Responder& res) {
    const bool openai = req.path == "/v1/chat/completions";
    const bool ollama = req.path == "/api/chat" || req.path == "/api/generate";
    if (!openai && !ollama) return false;

    Json body;
    std::string perr;
    if (!Json::parse(req.body, body, &perr)) {
        res.send_error(400, "invalid_request_error", "bad JSON: " + perr);
        return true;
    }

    const std::string model = body["model"].as_str();
    const SimModelProfile* mp = impl_->find(model);
    if (!mp) {
        res.send_error(404, "model_not_found", "no such model on this node: " + model);
        return true;
    }

    // Placement, not inference. Ollama expresses load and unload as
    // /api/generate with an empty prompt and a keep_alive — zero drops the
    // model, anything else holds it — so the simulated node has to understand
    // the same call or placement is untestable anywhere but on real hardware.
    const std::string prompt_probe = extract_prompt(body);
    if (ollama && body.has("keep_alive") && trim(prompt_probe).empty()) {
        const std::string keep = body["keep_alive"].is_str()
                                     ? body["keep_alive"].as_str()
                                     : std::to_string(body["keep_alive"].as_i64());
        const bool unload = (keep == "0" || keep == "0s");
        handle_placement(model, *mp, unload, res);
        return true;
    }

    // Ollama defaults to streaming, OpenAI to buffered.
    const bool stream = body.has("stream") ? body["stream"].as_bool() : ollama;
    const std::string prompt = prompt_probe;
    const uint32_t prompt_tokens = estimate_tokens(prompt);
    uint32_t output_tokens = requested_output_tokens(body);
    if (output_tokens == 0) {
        output_tokens = mp->output_tokens_max > 0
                            ? drawn_output_tokens(impl_->profile.seed, model, prompt,
                                                  mp->output_tokens_min,
                                                  mp->output_tokens_max)
                            : 128;
    }

    // --- occupy a slot; beyond engine_slots requests genuinely wait ---------
    {
        std::unique_lock<std::mutex> lock(impl_->mu);
        ++impl_->inflight;
        impl_->slot_cv.wait(lock, [&] {
            return impl_->slots_in_use < impl_->profile.engine_slots;
        });
        ++impl_->slots_in_use;
    }
    struct SlotRelease {
        Impl* impl;
        ~SlotRelease() {
            std::lock_guard<std::mutex> lock(impl->mu);
            --impl->slots_in_use;
            --impl->inflight;
            impl->slot_cv.notify_one();
        }
    } slot_release{impl_.get()};

    // --- load, evicting if necessary ---------------------------------------
    double load_ms = 0;
    std::vector<std::string> evicted;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        if (impl_->is_resident(model)) {
            for (auto& r : impl_->resident)
                if (r.name == model) r.last_used_ms = now_ms();
        } else {
            evicted = impl_->make_room(mp->footprint_bytes);
            if (impl_->vram_used + mp->footprint_bytes > impl_->profile.vram_total_bytes) {
                res.send_error(507, "insufficient_vram",
                               "model does not fit even with everything evicted");
                return true;
            }
            load_ms = impl_->jittered(static_cast<double>(mp->footprint_bytes) /
                                      impl_->profile.load_bandwidth_bytes_per_ms);
            impl_->resident.push_back({model, mp->footprint_bytes, now_ms()});
            impl_->vram_used += mp->footprint_bytes;
        }
    }
    sleep_ms(load_ms);

    // --- prefill ------------------------------------------------------------
    double prefill_ms;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        prefill_ms = impl_->jittered(static_cast<double>(prompt_tokens) /
                                     mp->prefill_tokens_per_ms);
    }
    sleep_ms(prefill_ms);

    // --- decode, with contention (§6.2) -------------------------------------
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        ++impl_->decoding;
    }
    struct DecodeRelease {
        Impl* impl;
        ~DecodeRelease() {
            std::lock_guard<std::mutex> lock(impl->mu);
            --impl->decoding;
        }
    } decode_release{impl_.get()};

    const std::string id = "chatcmpl-" + ulid();
    const int64_t created = now_ms() / 1000;

    auto per_token_ms = [&]() {
        std::lock_guard<std::mutex> lock(impl_->mu);
        const double concurrent = static_cast<double>(std::max(1u, impl_->decoding));
        const double rate = mp->decode_tokens_per_ms /
                            (1.0 + impl_->profile.contention_alpha * (concurrent - 1.0));
        return 1.0 / rate;
    };

    const std::string token_text = "tok ";
    std::string full_text;
    full_text.reserve(output_tokens * token_text.size());

    if (stream) {
        http::Headers h;
        h.set("Content-Type", openai ? "text/event-stream" : "application/x-ndjson");
        h.set("Cache-Control", "no-cache");
        if (!res.begin(200, h)) return true;

        for (uint32_t i = 0; i < output_tokens; ++i) {
            sleep_ms(per_token_ms());
            full_text += token_text;
            bool ok;
            if (openai) {
                Json delta = Json::object();
                delta["content"] = Json(token_text);
                Json choice = Json::object();
                choice["index"] = Json(0);
                choice["delta"] = std::move(delta);
                choice["finish_reason"] = Json();
                Json chunk = Json::object();
                chunk["id"] = Json(id);
                chunk["object"] = Json("chat.completion.chunk");
                chunk["created"] = Json(created);
                chunk["model"] = Json(model);
                chunk["choices"] = Json::array();
                chunk["choices"].push_back(std::move(choice));
                ok = res.sse("", chunk.dump());
            } else {
                Json msg = Json::object();
                msg["role"] = Json("assistant");
                msg["content"] = Json(token_text);
                Json chunk = Json::object();
                chunk["model"] = Json(model);
                chunk["created_at"] = Json(iso8601(now_ms()));
                chunk["message"] = std::move(msg);
                chunk["done"] = Json(false);
                ok = res.write(chunk.dump() + "\n");
            }
            if (!ok) return true;  // client vanished; slot released by the guards
        }

        if (openai) {
            Json choice = Json::object();
            choice["index"] = Json(0);
            choice["delta"] = Json::object();
            choice["finish_reason"] = Json("stop");
            Json usage = Json::object();
            usage["prompt_tokens"] = Json(prompt_tokens);
            usage["completion_tokens"] = Json(output_tokens);
            usage["total_tokens"] = Json(prompt_tokens + output_tokens);
            Json chunk = Json::object();
            chunk["id"] = Json(id);
            chunk["object"] = Json("chat.completion.chunk");
            chunk["created"] = Json(created);
            chunk["model"] = Json(model);
            chunk["choices"] = Json::array();
            chunk["choices"].push_back(std::move(choice));
            chunk["usage"] = std::move(usage);
            res.sse("", chunk.dump());
            res.sse("", "[DONE]");
        } else {
            Json done = Json::object();
            done["model"] = Json(model);
            done["created_at"] = Json(iso8601(now_ms()));
            done["message"] = Json::object();
            done["done"] = Json(true);
            done["done_reason"] = Json("stop");
            done["prompt_eval_count"] = Json(prompt_tokens);
            done["eval_count"] = Json(output_tokens);
            done["load_duration"] = Json(load_ms * 1e6);  // Ollama reports ns
            res.write(done.dump() + "\n");
        }
        res.end();
        return true;
    }

    for (uint32_t i = 0; i < output_tokens; ++i) {
        sleep_ms(per_token_ms());
        full_text += token_text;
    }

    if (openai) {
        Json msg = Json::object();
        msg["role"] = Json("assistant");
        msg["content"] = Json(full_text);
        Json choice = Json::object();
        choice["index"] = Json(0);
        choice["message"] = std::move(msg);
        choice["finish_reason"] = Json("stop");
        Json usage = Json::object();
        usage["prompt_tokens"] = Json(prompt_tokens);
        usage["completion_tokens"] = Json(output_tokens);
        usage["total_tokens"] = Json(prompt_tokens + output_tokens);
        Json out = Json::object();
        out["id"] = Json(id);
        out["object"] = Json("chat.completion");
        out["created"] = Json(created);
        out["model"] = Json(model);
        out["choices"] = Json::array();
        out["choices"].push_back(std::move(choice));
        out["usage"] = std::move(usage);
        res.send_json(200, out.dump());
    } else {
        Json msg = Json::object();
        msg["role"] = Json("assistant");
        msg["content"] = Json(full_text);
        Json out = Json::object();
        out["model"] = Json(model);
        out["created_at"] = Json(iso8601(now_ms()));
        out["message"] = std::move(msg);
        out["done"] = Json(true);
        out["done_reason"] = Json("stop");
        out["prompt_eval_count"] = Json(prompt_tokens);
        out["eval_count"] = Json(output_tokens);
        out["load_duration"] = Json(load_ms * 1e6);
        res.send_json(200, out.dump());
    }
    return true;
}

}  // namespace rf
