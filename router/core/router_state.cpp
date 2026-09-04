#include "router/core/router_state.h"

#include <algorithm>
#include <mutex>

#include "common/util.h"

namespace rf {

RouterState::RouterState(NodeRegistry& registry, std::unique_ptr<ICostModel> cost,
                         std::unique_ptr<IPolicy> policy, ScoringConfig scoring)
    : registry_(registry),
      cost_(std::move(cost)),
      policy_(std::move(policy)),
      scoring_(scoring) {}

Reservation RouterState::reserve_locked(const RequestFeatures& req,
                                        const std::vector<std::string>& skip) {
    Reservation r;
    r.policy_name = policy_->name();
    r.cost_model_name = cost_->name();

    // Snapshot is taken from the registry's own per-node locks, not ours; it is
    // a copy, so nothing here holds a poller up.
    std::vector<NodeState> nodes = registry_.snapshot();

    // Operator exclusions plus anything this request already failed on.
    std::vector<std::string> excluded = registry_.excluded_ids();
    excluded.insert(excluded.end(), skip.begin(), skip.end());

    // The policy applies the shared admission filters and the shared cost
    // model; all it decides is which admitted candidate wins.
    r.decision = policy_->select(req, nodes, ledger_, *cost_, scoring_);

    // Excluded nodes must still appear in the breakdown — a node that
    // influenced routing without showing up in the Decision is a bug (§10) and
    // that includes being excluded from it.
    for (auto& c : r.decision.candidates) {
        if (std::find(excluded.begin(), excluded.end(), c.node_id) == excluded.end())
            continue;
        if (c.node_id == r.decision.winner_node_id) continue;
        c.admitted = false;
        c.reason = AdmitReason::Excluded;
    }

    const Candidate* winner = r.decision.winner();
    if (!winner) {
        ++jobs_no_candidate_;
        return r;
    }

    const NodeState* node = nullptr;
    for (const auto& n : nodes)
        if (n.id == winner->node_id) node = &n;
    if (!node) return r;  // registry changed under us; treat as no candidate

    r.node_id = winner->node_id;
    r.was_resident = node->is_resident(req.model);
    r.footprint_bytes = cost_->footprint_bytes(req.model, *node, req.num_ctx);
    r.predicted_load_ms = winner->est.t_load_ms;
    r.would_evict = winner->would_evict;

    // We will load only if the model is not resident and nobody else is
    // already loading it here — in which case we queue behind their load
    // rather than reserving a second copy of the same VRAM (§6.3).
    const bool already_loading = ledger_.pending(node->id, req.model) != nullptr;
    r.will_load = !r.was_resident && node->residency_known;

    r.inflight_at_dispatch = ledger_.inflight(node->id);
    r.concurrent_decoders_at_dispatch = ledger_.concurrent_decoders(node->id);
    r.telemetry_ok = node->telemetry_ok;
    r.gpu_util = node->gpu_util;
    r.vram_free = node->vram_free_bytes;
    r.telemetry_backend = node->telemetry_backend;

    r.token = ledger_.reserve(node->id, req.model, r.footprint_bytes, r.will_load,
                              r.predicted_load_ms, now_ms());
    r.ok = true;
    ++jobs_dispatched_;

    if (already_loading)
        RF_DEBUG("job for '%s' queues behind an in-flight load on '%s'",
                 req.model.c_str(), node->id.c_str());
    return r;
}

Reservation RouterState::score_and_reserve(const RequestFeatures& req) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    return reserve_locked(req, {});
}

Reservation RouterState::score_and_reserve_excluding(
    const RequestFeatures& req, const std::vector<std::string>& skip) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    return reserve_locked(req, skip);
}

void RouterState::note_load_done(NodeLedger::Token token) {
    if (token == NodeLedger::kInvalid) return;
    std::unique_lock<std::shared_mutex> lock(mu_);
    ledger_.note_load_done(token);
}

void RouterState::note_decoding(NodeLedger::Token token) {
    if (token == NodeLedger::kInvalid) return;
    std::unique_lock<std::shared_mutex> lock(mu_);
    ledger_.note_decoding(token);
}

void RouterState::complete(NodeLedger::Token token, const TraceRecord& record) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    if (token != NodeLedger::kInvalid) ledger_.release(token);
    // Failed attempts are released but not learned from: their durations
    // describe a broken transfer, not the node's speed (TRACE-SCHEMA
    // "Derived quantities").
    if (record.outcome == Outcome::Ok) cost_->observe(record);
}

bool RouterState::set_policy(const std::string& name) {
    std::unique_ptr<IPolicy> next;
    if (name == "roundrobin-v1" || name == "roundrobin" || name == "round_robin")
        next = make_round_robin_policy();
    else if (name == "warmth-v1" || name == "warmth")
        next = make_warmth_policy();
    if (!next) return false;

    std::unique_lock<std::shared_mutex> lock(mu_);
    RF_INFO("policy switched: %s -> %s", policy_->name(), next->name());
    policy_ = std::move(next);
    return true;
}

std::string RouterState::policy_name() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return policy_->name();
}

std::string RouterState::cost_model_name() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return cost_->name();
}

Json RouterState::stats_json() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    Json j = Json::object();
    j["policy"] = Json(policy_->name());
    j["cost_model"] = Json(cost_->name());
    j["jobs_dispatched"] = Json(jobs_dispatched_);
    j["jobs_no_candidate"] = Json(jobs_no_candidate_);
    j["ledger"] = ledger_.to_json();

    Json s = Json::object();
    s["evict_weight"] = Json(scoring_.evict_weight);
    s["warm_window_ms"] = Json(scoring_.warm_window_ms);
    s["thermal_penalty"] = Json(scoring_.thermal_penalty);
    s["node_stale_ms"] = Json(scoring_.node_stale_ms);
    s["footprint_margin"] = Json(scoring_.footprint_margin);
    s["contention_alpha"] = Json(scoring_.contention_alpha);
    j["scoring"] = std::move(s);
    return j;
}

}  // namespace rf
