#include "router/core/ledger.h"

#include "common/util.h"

namespace rf {

const NodeLedger::NodeAccount* NodeLedger::find(const std::string& node_id) const {
    auto it = nodes_.find(node_id);
    return it == nodes_.end() ? nullptr : &it->second;
}

NodeLedger::Token NodeLedger::reserve(const std::string& node_id,
                                      const std::string& model,
                                      uint64_t footprint_bytes, bool will_load,
                                      double eta_ms, int64_t now_ms) {
    const Token token = next_token_++;
    Entry e;
    e.node_id = node_id;
    e.model = model;
    e.footprint_bytes = footprint_bytes;
    e.loading = will_load;

    NodeAccount& acc = account(node_id);
    ++acc.inflight;
    ++acc.busy_models[model];

    if (will_load) {
        acc.reserved_vram += footprint_bytes;
        e.load.model = model;
        e.load.started_ms = now_ms;
        e.load.eta_ms = eta_ms;
        auto it = acc.pending_loads.find(model);
        if (it == acc.pending_loads.end()) {
            acc.pending_loads.emplace(model, std::make_pair(e.load, 1u));
        } else {
            // A second request for a model already loading here does not start
            // a second load; it waits behind the first. Counting the waiters
            // keeps the pending entry alive until the last of them is done.
            ++it->second.second;
        }
    }

    entries_.emplace(token, std::move(e));
    return token;
}

void NodeLedger::note_load_done(Token token) {
    auto it = entries_.find(token);
    if (it == entries_.end() || !it->second.loading) return;
    Entry& e = it->second;
    e.loading = false;

    NodeAccount& acc = account(e.node_id);
    acc.reserved_vram = acc.reserved_vram > e.footprint_bytes
                            ? acc.reserved_vram - e.footprint_bytes
                            : 0;
    auto pit = acc.pending_loads.find(e.model);
    if (pit != acc.pending_loads.end() && --pit->second.second == 0)
        acc.pending_loads.erase(pit);
}

void NodeLedger::note_decoding(Token token) {
    auto it = entries_.find(token);
    if (it == entries_.end() || it->second.decoding) return;
    it->second.decoding = true;
    ++account(it->second.node_id).decoders;
}

void NodeLedger::release(Token token) {
    auto it = entries_.find(token);
    if (it == entries_.end()) return;
    Entry e = it->second;
    entries_.erase(it);

    auto nit = nodes_.find(e.node_id);
    if (nit == nodes_.end()) return;
    NodeAccount& acc = nit->second;

    if (acc.inflight > 0) --acc.inflight;
    if (e.decoding && acc.decoders > 0) --acc.decoders;

    auto bit = acc.busy_models.find(e.model);
    if (bit != acc.busy_models.end() && --bit->second == 0) acc.busy_models.erase(bit);

    // Released after the load finished means the engine held this model to do
    // the work, whatever became of the reply afterwards. Released while still
    // loading means it may never have become resident, so that is not evidence
    // and is not recorded (D36).
    if (!e.loading) last_served_[e.node_id + '\x1f' + e.model] = now_ms();

    if (e.loading) {
        // Released while still loading: the request failed or was aborted.
        acc.reserved_vram = acc.reserved_vram > e.footprint_bytes
                                ? acc.reserved_vram - e.footprint_bytes
                                : 0;
        auto pit = acc.pending_loads.find(e.model);
        if (pit != acc.pending_loads.end() && --pit->second.second == 0)
            acc.pending_loads.erase(pit);
    }

    // Keep empty accounts out of the map so /admin/stats shows live nodes only.
    if (acc.inflight == 0 && acc.decoders == 0 && acc.reserved_vram == 0 &&
        acc.pending_loads.empty() && acc.busy_models.empty())
        nodes_.erase(nit);
}

uint32_t NodeLedger::inflight(const std::string& node_id) const {
    const NodeAccount* acc = find(node_id);
    return acc ? acc->inflight : 0;
}

uint32_t NodeLedger::concurrent_decoders(const std::string& node_id) const {
    const NodeAccount* acc = find(node_id);
    return acc ? acc->decoders : 0;
}

uint64_t NodeLedger::reserved_vram_bytes(const std::string& node_id) const {
    const NodeAccount* acc = find(node_id);
    return acc ? acc->reserved_vram : 0;
}

const PendingLoad* NodeLedger::pending(const std::string& node_id,
                                       const std::string& model) const {
    const NodeAccount* acc = find(node_id);
    if (!acc) return nullptr;
    auto it = acc->pending_loads.find(model);
    return it == acc->pending_loads.end() ? nullptr : &it->second.first;
}

bool NodeLedger::model_busy(const std::string& node_id, const std::string& model) const {
    const NodeAccount* acc = find(node_id);
    if (!acc) return false;
    return acc->busy_models.count(model) > 0;
}

int64_t NodeLedger::last_served_ms(const std::string& node_id,
                                   const std::string& model) const {
    auto it = last_served_.find(node_id + '\x1f' + model);
    return it == last_served_.end() ? 0 : it->second;
}

uint32_t NodeLedger::models_served_since(const std::string& node_id,
                                         const std::string& except_model,
                                         int64_t since_ms) const {
    const std::string prefix = node_id + '';
    uint32_t n = 0;
    // The map is keyed node|model and sorted, so the node's entries are one
    // contiguous run.
    for (auto it = last_served_.lower_bound(prefix);
         it != last_served_.end() && it->first.compare(0, prefix.size(), prefix) == 0;
         ++it) {
        if (it->second <= since_ms) continue;
        if (it->first.compare(prefix.size(), std::string::npos, except_model) == 0)
            continue;
        ++n;
    }
    return n;
}

Json NodeLedger::to_json() const {
    Json out = Json::object();
    for (const auto& kv : nodes_) {
        const NodeAccount& acc = kv.second;
        Json n = Json::object();
        n["inflight"] = Json(acc.inflight);
        n["decoders"] = Json(acc.decoders);
        n["reserved_vram_bytes"] = Json(acc.reserved_vram);
        Json pending = Json::array();
        for (const auto& p : acc.pending_loads) {
            Json e = Json::object();
            e["model"] = Json(p.first);
            e["started_ms"] = Json(p.second.first.started_ms);
            e["eta_ms"] = Json(p.second.first.eta_ms);
            e["waiters"] = Json(p.second.second);
            pending.push_back(std::move(e));
        }
        n["pending_loads"] = std::move(pending);
        out[kv.first] = std::move(n);
    }
    return out;
}

}  // namespace rf
