// LearnedCostModel ("learned-v1") — Phase 2 (ARCHITECTURE §8).
//
// Every quantity is an EWMA seeded from the static table and replaced
// independently as evidence arrives. That independence matters: a node
// typically has hundreds of generations behind its decode rate and three cold
// starts behind its load bandwidth, so one converged number should not have to
// wait on another.
//
// What is learned, and from which records:
//
//   prefill_rate(node, model)   warm, uncontended records only. A warm record
//                               has load_ms == 0 by definition, so its ttft is
//                               prefill alone (D19's bootstrap).
//   decode_rate(node, model)    records with one decoder only. A rate measured
//                               while the GPU was shared is not the rate.
//   alpha(node)                 records with more than one decoder, by solving
//                               §6.2's contention equation for alpha. This is
//                               the only thing contended records teach.
//   load_bandwidth(node)        engine-reported load times, and — weighted
//                               lower — load derived from a cold start's ttft
//                               once prefill is known.
//   output_len(model, role)     tokens generated, with a companion EWMA of
//                               absolute error that becomes sigma. §8 insists
//                               these two errors stay apart, and they do.
//   warm_job_ms(node)           for T_queue, excluding cold starts (D6).
//
// The split is the point. Fitting one function to "how long will this take"
// would let a bad load estimate corrupt the decode rate, and there would be no
// way to see which half was wrong.

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include "common/util.h"
#include "router/core/interfaces.h"
#include "router/cost/seeds.h"

namespace rf {
namespace {

// An exponentially weighted mean plus the count behind it. The count is not
// decoration: it decides whether a value is trusted over its seed, and it is
// what Confidence reports to the UI.
struct Ewma {
    double value = 0;
    uint32_t samples = 0;

    void add(double x, double halflife) {
        if (!std::isfinite(x) || x <= 0) return;
        if (samples == 0) {
            value = x;
        } else {
            const double w = 2.0 / (std::max(1.0, halflife) + 1.0);
            value = w * x + (1.0 - w) * value;
        }
        ++samples;
    }
    bool has(uint32_t min_samples) const { return samples >= min_samples; }
};

// A rate is used over its seed only once a few observations agree. One sample
// from a request that happened to hit a cold page cache should not redefine a
// node.
constexpr uint32_t kMinSamples = 3;

Confidence confidence_for(uint32_t samples) {
    if (samples < kMinSamples) return Confidence::Seeded;
    if (samples < 20) return Confidence::Learning;
    return Confidence::Converged;
}

std::string key2(const std::string& a, const std::string& b) { return a + '\x1f' + b; }

class LearnedCostModel : public ICostModel {
public:
    explicit LearnedCostModel(const ScoringConfig& scoring)
        : scoring_(scoring), seed_(make_static_cost_model(scoring)) {}

    const char* name() const override { return "learned-v1"; }

    uint64_t footprint_bytes(const std::string& model, const NodeState& node,
                             uint32_t num_ctx) const override {
        // Observed footprint would be better, but nothing in a v1 trace records
        // the VRAM delta across a load, so this stays seeded and says so. It is
        // the one quantity Phase 2 cannot improve without a schema change, and
        // pretending otherwise would be the easiest lie to tell here.
        return seed_footprint_bytes(model, node, num_ctx, scoring_);
    }

    OutputPrediction predict_output(const RequestFeatures& req) const override {
        // A caller-supplied cap is a real upper bound and beats anything we
        // learned; it is the request's own statement about itself.
        if (req.predicted_output_tokens > 0) {
            OutputPrediction p;
            p.tokens = req.predicted_output_tokens;
            p.sigma = req.predicted_output_sigma;
            p.conf = Confidence::Seeded;
            return p;
        }
        const Ewma* len = find(output_len_, key2(req.model, req.role_hint));
        if (!len || !len->has(kMinSamples)) {
            // Fall back to the per-model average before the seed: a role we have
            // not seen on a model we have is still informative.
            len = find(output_len_, key2(req.model, std::string()));
        }
        if (!len || !len->has(kMinSamples)) return seed_->predict_output(req);

        OutputPrediction p;
        p.tokens = static_cast<uint32_t>(std::max(1.0, len->value));
        const Ewma* err = find(output_err_, key2(req.model, req.role_hint));
        if (!err || !err->has(kMinSamples)) err = find(output_err_, key2(req.model, ""));
        // Sigma is the measured absolute error, not a guessed fraction. This is
        // what narrows the noise band as the model learns, which is what turns
        // within_noise decisions into real ones (D8).
        p.sigma = err && err->has(kMinSamples)
                      ? static_cast<float>(err->value)
                      : static_cast<float>(len->value * 0.5);
        p.conf = confidence_for(len->samples);
        return p;
    }

    Estimate estimate(const RequestFeatures& req, const NodeState& node,
                      const LedgerView& ledger, const EvictionPlan& evict,
                      int64_t now_ms) const override {
        const uint64_t footprint = footprint_bytes(req.model, node, req.num_ctx);
        const SeedRates seeded = seed_rates(node, footprint, scoring_);
        const std::string nm = key2(node.id, req.model);

        Estimate e;
        uint32_t worst_samples = UINT32_MAX;

        // --- decode ----------------------------------------------------------
        double decode = seeded.decode_tokens_per_ms;
        const Ewma* d = find(decode_rate_, nm);
        if (d && d->has(kMinSamples)) {
            decode = d->value;
            worst_samples = std::min(worst_samples, d->samples);
        }

        double alpha = scoring_.contention_alpha;
        const Ewma* a = find(alpha_, node.id);
        if (a && a->has(kMinSamples)) alpha = a->value;

        const uint32_t decoders = ledger.concurrent_decoders(node.id);
        const double concurrent = static_cast<double>(decoders) + 1.0;
        const double effective_decode = decode / (1.0 + alpha * (concurrent - 1.0));

        const OutputPrediction out = predict_output(req);
        e.t_decode_ms =
            static_cast<double>(out.tokens) / std::max(1e-9, effective_decode);

        // --- prefill ---------------------------------------------------------
        double prefill = seeded.prefill_tokens_per_ms;
        const Ewma* p = find(prefill_rate_, nm);
        if (p && p->has(kMinSamples)) {
            prefill = p->value;
            worst_samples = std::min(worst_samples, p->samples);
        }
        e.t_prefill_ms =
            static_cast<double>(req.prompt_tokens) / std::max(1e-9, prefill);

        // --- load ------------------------------------------------------------
        double load_bw = seeded.load_bytes_per_ms;
        const Ewma* l = find(load_bw_, node.id);
        const bool load_learned = l && l->has(kMinSamples);
        if (load_learned) load_bw = l->value;

        if (!node.residency_known) {
            e.t_load_ms = 0;
            e.omitted_terms |= kTermLoad;
        } else if (node.is_resident(req.model)) {
            e.t_load_ms = 0;
        } else if (const PendingLoad* pend = ledger.pending(node.id, req.model)) {
            const double elapsed = static_cast<double>(now_ms - pend->started_ms);
            e.t_load_ms = std::max(0.0, pend->eta_ms - elapsed);
        } else {
            e.t_load_ms = static_cast<double>(footprint) / std::max(1.0, load_bw);
        }

        // --- queue (§6.2, D6) -------------------------------------------------
        const uint32_t slots = std::max(1u, node.engine_slots);
        const uint32_t inflight = ledger.inflight(node.id);
        if (inflight < slots) {
            e.t_queue_ms = 0;
        } else {
            // Measured warm job duration where we have it: it captures per-job
            // overhead the two rates do not, and it is already cold-start-free.
            const Ewma* w = find(warm_job_ms_, node.id);
            const double warm_job_ms = (w && w->has(kMinSamples))
                                           ? w->value
                                           : e.t_prefill_ms + e.t_decode_ms;
            const uint32_t ahead = inflight - slots + 1;
            const double rounds =
                std::ceil(static_cast<double>(ahead) / static_cast<double>(slots));
            e.t_queue_ms = rounds * warm_job_ms;
        }

        // --- eviction (§6.2, D7) ----------------------------------------------
        if (evict.bytes > 0 && evict.evicts_warm && scoring_.evict_weight > 0) {
            e.t_evict_ms = static_cast<double>(evict.bytes) /
                           std::max(1.0, load_bw) * scoring_.evict_weight;
        }

        // --- thermal ----------------------------------------------------------
        if (scoring_.thermal_penalty) {
            if (node.telemetry_ok && node.power_cap_watts > 0 && node.power_watts > 0) {
                const double headroom =
                    1.0 - std::min(1.0, static_cast<double>(node.power_watts) /
                                            static_cast<double>(node.power_cap_watts));
                e.t_decode_ms += scoring_.thermal_weight_ms * (1.0 - headroom);
            } else {
                e.omitted_terms |= kTermThermal;
            }
        }

        // --- uncertainty (D8) -------------------------------------------------
        // The load residual shrinks once load bandwidth is measured rather than
        // assumed. That is most of what Phase 2 buys the noise check: a cold
        // candidate stops carrying a 30% band it did not earn.
        const double sigma_decode = out.sigma / std::max(1e-9, effective_decode);
        const double sigma_load = e.t_load_ms * (load_learned ? 0.12 : 0.30);
        e.sigma_ms = std::sqrt(sigma_decode * sigma_decode + sigma_load * sigma_load);

        e.samples = worst_samples == UINT32_MAX ? 0 : worst_samples;
        e.conf = confidence_for(e.samples);
        return e;
    }

    void observe(const TraceRecord& r) override {
        if (r.outcome != Outcome::Ok || r.node_id.empty()) return;
        const double hl = scoring_.ewma_halflife;
        const std::string nm = key2(r.node_id, r.model);

        // Output length, and its error, kept apart from timing error (§8).
        if (r.has_output_tokens && r.output_tokens > 0) {
            const double actual = r.output_tokens;
            output_len_[key2(r.model, r.role_hint)].add(actual, hl);
            output_len_[key2(r.model, std::string())].add(actual, hl);
            if (r.predicted_output_tokens > 0) {
                const double err = std::fabs(actual - r.predicted_output_tokens);
                // A perfect prediction gives an error of zero, which Ewma::add
                // refuses as non-positive — so it is floored at one token. The
                // alternative is a sigma that collapses to zero and a scheduler
                // that claims certainty it cannot have.
                output_err_[key2(r.model, r.role_hint)].add(std::max(1.0, err), hl);
                output_err_[key2(r.model, std::string())].add(std::max(1.0, err), hl);
            }
        }

        if (!r.has_ttft) return;  // nothing below can be derived without it

        const double decode_ms = r.total_ms - r.ttft_ms;
        const double prefill_ms = r.ttft_ms - r.load_ms - r.queue_wait_ms;

        // Warm jobs feed the queue term, cold ones would inflate it (D6).
        if (r.was_resident && r.total_ms > 0) warm_job_ms_[r.node_id].add(r.total_ms, hl);

        // Prefill: warm and unqueued only. `inflight_at_dispatch == 0` is
        // stricter than the schema's `< engine_slots` because a v1 record does
        // not carry the slot count; erring strict costs samples, not accuracy.
        if (r.was_resident && r.inflight_at_dispatch == 0 && prefill_ms > 0 &&
            r.has_prompt_tokens_actual && r.prompt_tokens_actual > 0) {
            prefill_rate_[nm].add(r.prompt_tokens_actual / prefill_ms, hl);
        }

        if (!r.has_output_tokens || r.output_tokens == 0 || decode_ms <= 0) return;
        const double observed_decode = r.output_tokens / decode_ms;

        if (r.concurrent_decoders_at_dispatch <= 1) {
            decode_rate_[nm].add(observed_decode, hl);
        } else {
            // Contended records teach alpha, never the base rate. Solving
            // §6.2's equation needs a base rate to compare against, so this
            // only works once the uncontended rate exists — which is the right
            // order anyway.
            const Ewma* base = find(decode_rate_, nm);
            if (base && base->has(kMinSamples) && observed_decode > 0) {
                const double n = r.concurrent_decoders_at_dispatch;
                const double solved = (base->value / observed_decode - 1.0) / (n - 1.0);
                // A negative solution means the contended run was *faster* than
                // the uncontended baseline — noise, not physics. Clamped rather
                // than dropped so a node under sustained load still converges.
                alpha_[r.node_id].add(std::max(0.05, std::min(4.0, solved)), hl);
            }
        }

        // Load bandwidth. An engine-reported load time is the real measurement;
        // a derived one is a subtraction with a learned rate inside it, so it
        // is only used when the engine said nothing and the node was idle.
        const uint64_t footprint_guess = r.footprint_bytes;
        if (footprint_guess == 0) return;  // older record; nothing to divide by
        if (r.has_load_ms && r.load_ms > 0) {
            load_bw_[r.node_id].add(footprint_guess / r.load_ms, hl);
        } else if (!r.was_resident && !r.has_load_ms && r.inflight_at_dispatch == 0) {
            const Ewma* pr = find(prefill_rate_, nm);
            if (pr && pr->has(kMinSamples) && r.has_prompt_tokens_actual) {
                const double prefill_est = r.prompt_tokens_actual / pr->value;
                const double derived = r.ttft_ms - r.queue_wait_ms - prefill_est;
                if (derived > 0) load_bw_[r.node_id].add(footprint_guess / derived, hl);
            }
        }
    }

private:
    static const Ewma* find(const std::map<std::string, Ewma>& m,
                            const std::string& k) {
        auto it = m.find(k);
        return it == m.end() ? nullptr : &it->second;
    }

    ScoringConfig scoring_;
    std::unique_ptr<ICostModel> seed_;

    std::map<std::string, Ewma> decode_rate_;    // node|model
    std::map<std::string, Ewma> prefill_rate_;   // node|model
    std::map<std::string, Ewma> load_bw_;        // node
    std::map<std::string, Ewma> alpha_;          // node
    std::map<std::string, Ewma> warm_job_ms_;    // node
    std::map<std::string, Ewma> output_len_;     // model|role
    std::map<std::string, Ewma> output_err_;     // model|role
};

}  // namespace

std::unique_ptr<ICostModel> make_learned_cost_model(const ScoringConfig& scoring) {
    return std::unique_ptr<ICostModel>(new LearnedCostModel(scoring));
}

}  // namespace rf
