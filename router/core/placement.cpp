#include "router/core/placement.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "common/config.h"
#include "common/util.h"

namespace rf {
namespace {

std::string key(const std::string& node, const std::string& model) {
    return node + '\x1f' + model;
}

}  // namespace

PlacementConfig PlacementConfig::from_config(const Config& cfg) {
    PlacementConfig p;
    p.enabled = cfg.get_bool("placement.enabled", p.enabled);
    p.interval_ms = static_cast<int>(cfg.get_u32("placement.interval_ms",
                                                 p.interval_ms));
    p.window_ms = cfg.get_int("placement.window_ms", p.window_ms);
    p.min_demand = cfg.get_u32("placement.min_demand", p.min_demand);
    p.evict_margin = cfg.get_num("placement.evict_margin", p.evict_margin);
    p.preload_timeout_ms = static_cast<int>(
        cfg.get_u32("placement.preload_timeout_ms", p.preload_timeout_ms));
    return p;
}

PlacementManager::PlacementManager(NodeRegistry& registry, RouterState& state,
                                   PlacementConfig config, std::string node_token)
    : registry_(registry),
      state_(state),
      config_(config),
      node_token_(std::move(node_token)),
      ollama_(make_ollama_adapter()) {}

PlacementManager::~PlacementManager() { stop(); }

void PlacementManager::observe(const TraceRecord& record) {
    // Only successful jobs are demand. A request that failed to dispatch says
    // nothing about which model a node should be holding — and counting it
    // would make a broken node attract preloads.
    if (record.outcome != Outcome::Ok || record.node_id.empty()) return;
    std::lock_guard<std::mutex> lock(mu_);
    Demand& d = demand_[key(record.node_id, record.model)];
    ++d.count;
    d.last_ms = now_ms();
}

void PlacementManager::start() {
    if (!config_.enabled || running_.exchange(true)) return;
    RF_INFO("placement manager on: every %d ms, demand window %lld ms, "
            "preload after %u requests",
            config_.interval_ms, static_cast<long long>(config_.window_ms),
            config_.min_demand);
    thread_ = std::thread([this] { loop(); });
}

void PlacementManager::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void PlacementManager::loop() {
    while (running_.load()) {
        // Sleep first: at start-up nothing has been requested yet, so there is
        // no demand to act on and a cycle would only cost a poll.
        for (int slept = 0; slept < config_.interval_ms && running_.load();
             slept += 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!running_.load()) return;

        for (const NodeState& node : registry_.snapshot()) {
            if (!running_.load()) return;
            consider(node);
        }
    }
}

void PlacementManager::consider(const NodeState& node) {
    if (!node.engine_healthy || !node.residency_known) return;

    const NodeConfig* cfg = registry_.config_for(node.id);
    if (!cfg || cfg->excluded) return;
    if (!make_engine_adapter(node.engine)) return;  // nothing we can place on

    // A preload occupies a slot. Doing it while the node is working steals
    // capacity from the requests placement exists to help, and shows up as the
    // p95 regression the exit criterion forbids. The router's own ledger is the
    // authority here, not the engine's poll-delayed self-report (§6.3).
    if (state_.inflight_for(node.id) > 0 || node.inflight_reported > 0) {
        std::lock_guard<std::mutex> lock(mu_);
        ++skipped_busy_;
        return;
    }

    const int64_t now = now_ms();

    // What this node is actually being asked for, most-wanted first.
    struct Want {
        std::string model;
        uint32_t count;
        int64_t last_ms;
    };
    std::vector<Want> wanted;
    std::map<std::string, Demand> snapshot;
    {
        std::lock_guard<std::mutex> lock(mu_);
        snapshot = demand_;
    }
    for (const auto& kv : snapshot) {
        const size_t sep = kv.first.find('\x1f');
        if (sep == std::string::npos || kv.first.compare(0, sep, node.id) != 0)
            continue;
        if (now - kv.second.last_ms > config_.window_ms) continue;
        if (kv.second.count < config_.min_demand) continue;
        wanted.push_back({kv.first.substr(sep + 1), kv.second.count,
                          kv.second.last_ms});
    }
    if (wanted.empty()) {
        RF_DEBUG("placement %s: idle, but nothing has %u+ requests in the window",
                 node.id.c_str(), config_.min_demand);
        return;
    }
    std::sort(wanted.begin(), wanted.end(), [](const Want& a, const Want& b) {
        return a.count > b.count;
    });

    // The most-wanted model that is not already here and could be.
    const Want* target = nullptr;
    for (const Want& w : wanted) {
        if (node.is_resident(w.model)) continue;
        if (!node.has_on_disk(w.model)) continue;
        target = &w;
        break;
    }
    if (!target) {
        std::string detail;
        for (const Want& w : wanted) {
            if (!detail.empty()) detail += ", ";
            detail += w.model + "=" + std::to_string(w.count) +
                      (node.is_resident(w.model) ? " (resident)" : "") +
                      (node.has_on_disk(w.model) ? "" : " (not on disk)");
        }
        RF_DEBUG("placement %s: idle, wanted [%s], nothing to preload",
                 node.id.c_str(), detail.c_str());
        return;
    }
    RF_DEBUG("placement %s: idle, target %s (%u requests), free %llu, needs %llu",
             node.id.c_str(), target->model.c_str(), target->count,
             static_cast<unsigned long long>(node.vram_free_bytes),
             static_cast<unsigned long long>(
                 state_.footprint_for(target->model, node)));

    const uint64_t footprint = state_.footprint_for(target->model, node);
    if (footprint == 0 || footprint > node.vram_total_bytes) return;

    EngineTarget engine;
    engine.host = cfg->dispatch_host();
    engine.port = cfg->dispatch_port();
    engine.kind = node.engine;
    if (!cfg->dispatch_direct_to_engine) engine.token = node_token_;

    // Make room only if room is needed, and only by displacing something the
    // cluster wants *less*.
    //
    // An absolute staleness cutoff does not work here, which took a measurement
    // to notice: with any cutoff longer than a busy period, nothing is ever
    // stale, no eviction is ever permitted, and no preload that needs room can
    // happen — so the manager sat inert through exactly the memory pressure it
    // exists for. The comparison has to be relative.
    //
    // Evicting the least-wanted resident for a clearly hotter arrival *is* the
    // claim Phase 3 is testing: that frequency is a better eviction signal than
    // the engine's recency. The margin keeps it from thrashing on near-ties,
    // where swapping costs a load and buys nothing.
    //
    // "Room" is not only bytes. An engine that keeps a fixed number of models
    // is full at that number, and the preload becomes a swap the manager never
    // asked for -- measured on real hardware as six or seven preloads a run,
    // zero evictions counted, and cold starts moved from one model to the other
    // rather than removed (D38). Making the same test cover both cases puts
    // that swap back under the margin rule below, where it can be declined.
    const bool crowded = at_residency_limit(node, target->model);
    if (node.vram_free_bytes < footprint || crowded) {
        struct Victim {
            const ResidentModel* model;
            uint32_t demand;
        };
        std::vector<Victim> victims;
        for (const auto& m : node.models_resident) {
            const auto it = snapshot.find(key(node.id, m.name));
            const uint32_t d = (it != snapshot.end() &&
                                now - it->second.last_ms <= config_.window_ms)
                                   ? it->second.count
                                   : 0;
            victims.push_back({&m, d});
        }
        std::sort(victims.begin(), victims.end(),
                  [](const Victim& a, const Victim& b) { return a.demand < b.demand; });

        uint64_t freed = node.vram_free_bytes;
        size_t resident_after = node.models_resident.size();
        const size_t room_for = node.models_resident_limit > 0
                                    ? node.models_resident_limit - 1
                                    : resident_after;
        std::vector<const ResidentModel*> chosen;
        for (const Victim& v : victims) {
            if (freed >= footprint && resident_after <= room_for) break;
            // Only displace something the arrival clearly beats. Without this a
            // pair of equally-wanted models would swap places forever, each
            // eviction paying a load for no gain.
            if (static_cast<double>(target->count) <
                static_cast<double>(v.demand) * config_.evict_margin) {
                break;
            }
            chosen.push_back(v.model);
            freed += v.model->vram_bytes;
            --resident_after;
        }
        // Nothing here is wanted enough less than the arrival to justify the
        // swap. On a crowded engine that is the common answer, and declining is
        // the whole point: a preload that costs a warm model its place is not
        // a preload.
        if (freed < footprint || resident_after > room_for) {
            RF_DEBUG("placement %s: %s wanted %u, but nothing resident is wanted "
                     "enough less to displace",
                     node.id.c_str(), target->model.c_str(), target->count);
            return;
        }

        for (const ResidentModel* victim : chosen) {
            std::string err;
            if (ollama_->evict(engine, victim->name, 30000, &err)) {
                std::lock_guard<std::mutex> lock(mu_);
                ++evictions_;
                last_action_ = "evicted " + victim->name + " from " + node.id;
                RF_INFO("placement: evicted %s from %s to make room for %s",
                        victim->name.c_str(), node.id.c_str(),
                        target->model.c_str());
            } else {
                std::lock_guard<std::mutex> lock(mu_);
                ++failures_;
                RF_WARN("placement: could not evict %s from %s: %s",
                        victim->name.c_str(), node.id.c_str(), err.c_str());
                return;
            }
        }
    }

    // One action per node per cycle. A burst of preloads would monopolise the
    // cluster doing work nobody asked for yet.
    std::string err;
    const int64_t started = mono_ms();
    if (ollama_->preload(engine, target->model, config_.preload_timeout_ms, &err)) {
        std::lock_guard<std::mutex> lock(mu_);
        ++preloads_;
        last_action_ = "preloaded " + target->model + " on " + node.id;
        RF_INFO("placement: preloaded %s on %s in %lld ms (%u recent requests)",
                target->model.c_str(), node.id.c_str(),
                static_cast<long long>(mono_ms() - started), target->count);
    } else {
        std::lock_guard<std::mutex> lock(mu_);
        ++failures_;
        RF_WARN("placement: could not preload %s on %s: %s", target->model.c_str(),
                node.id.c_str(), err.c_str());
    }
}

Json PlacementManager::stats_json() const {
    std::lock_guard<std::mutex> lock(mu_);
    Json j = Json::object();
    j["enabled"] = Json(config_.enabled);
    j["preloads"] = Json(preloads_);
    j["evictions"] = Json(evictions_);
    j["failures"] = Json(failures_);
    j["skipped_busy"] = Json(skipped_busy_);
    j["last_action"] = last_action_.empty() ? Json() : Json(last_action_);

    Json d = Json::object();
    for (const auto& kv : demand_) {
        const size_t sep = kv.first.find('\x1f');
        if (sep == std::string::npos) continue;
        Json e = Json::object();
        e["node"] = Json(kv.first.substr(0, sep));
        e["model"] = Json(kv.first.substr(sep + 1));
        e["count"] = Json(kv.second.count);
        e["last_ms"] = Json(kv.second.last_ms);
        d[kv.first.substr(0, sep) + "/" + kv.first.substr(sep + 1)] = std::move(e);
    }
    j["demand"] = std::move(d);
    return j;
}

}  // namespace rf
