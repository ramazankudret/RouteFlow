// RouteFlow — admission, scoring and policy tests.
//
// These check the claims the project rests on, in the one place where they can
// be checked exactly: no sockets, no engines, no timing. A node's state is a
// struct, the ledger is a struct, and the answer is arithmetic.
//
// The central case is the one from ARCHITECTURE §1 — a busy weak node that
// holds the model warm finishing before an idle strong node that has to load
// it — and its mirror, where the generation is long enough that loading on the
// fast node is genuinely the better call. A policy that got the first right and
// the second wrong would not be a scheduler, it would be a warmth bias.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "common/types.h"
#include "common/util.h"
#include "router/core/admission.h"
#include "router/core/interfaces.h"
#include "router/core/ledger.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void check_eq(const std::string& got, const std::string& want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s\n        got:  %s\n        want: %s\n", what.c_str(),
                    got.c_str(), want.c_str());
    }
}

void section(const char* name) { std::printf("\n%s\n", name); }

// --- fixtures ---------------------------------------------------------------

constexpr uint64_t kGB = 1000ULL * 1000 * 1000;

rf::NodeState make_node(const std::string& id, const std::string& gpu,
                        uint64_t vram_total, uint64_t vram_free) {
    rf::NodeState n;
    n.id = id;
    n.gpu_name = gpu;
    n.telemetry_backend = "nvml";
    n.telemetry_ok = true;
    n.vram_total_bytes = vram_total;
    n.vram_free_bytes = vram_free;
    n.engine = rf::EngineKind::Ollama;
    n.engine_healthy = true;
    n.engine_slots = 1;
    n.residency_known = true;
    n.sampled_at_ms = rf::now_ms();
    return n;
}

void add_disk(rf::NodeState& n, const std::string& model, uint64_t bytes) {
    n.models_on_disk.push_back(model);
    n.model_disk_bytes.emplace_back(model, bytes);
}

void add_resident(rf::NodeState& n, const std::string& model, uint64_t bytes,
                  int64_t last_used_ms) {
    n.models_resident.push_back({model, bytes, last_used_ms});
}

rf::ScoringConfig bench_scoring() {
    rf::ScoringConfig s;
    // The same seeds bench/router.json ships, so the test and the benchmark
    // are reasoning about the same cluster.
    s.gpu_seeds = {
        {"4060 laptop", 195e6, 0.85e6, 33},
        {"orin nx", 90e6, 1.10e6, 33},
        {"2050", 65e6, 0.45e6, 33},
    };
    return s;
}

// desktop: fast, 8 GB, holds one big model at a time, nothing resident.
// jetson:  slow, 15 GB, planner already warm.
// laptop:  weak, 6 GB, cannot fit the planner at all.
std::vector<rf::NodeState> make_cluster() {
    rf::NodeState desktop = make_node("sim-desktop",
                                      "NVIDIA GeForce RTX 4060 Laptop GPU",
                                      8 * kGB, 8 * kGB);
    add_disk(desktop, "planner:12b", 6800 * 1000 * 1000ULL);
    add_disk(desktop, "worker:3b", 1900 * 1000 * 1000ULL);

    rf::NodeState jetson = make_node("sim-jetson", "NVIDIA Jetson Orin NX 16GB",
                                     15 * kGB, 8 * kGB);
    add_disk(jetson, "planner:12b", 6800 * 1000 * 1000ULL);
    add_disk(jetson, "worker:3b", 1900 * 1000 * 1000ULL);
    add_resident(jetson, "planner:12b", 7 * kGB, rf::now_ms());

    rf::NodeState laptop = make_node("sim-laptop", "NVIDIA GeForce RTX 2050 Laptop GPU",
                                     6 * kGB, 4 * kGB);
    add_disk(laptop, "planner:12b", 6800 * 1000 * 1000ULL);
    add_disk(laptop, "worker:3b", 1900 * 1000 * 1000ULL);
    add_resident(laptop, "worker:3b", 2 * kGB, rf::now_ms());

    return {desktop, jetson, laptop};
}

rf::RequestFeatures planner_request(uint32_t output_tokens) {
    rf::RequestFeatures f;
    f.model = "planner:12b";
    f.prompt_tokens = 1200;
    f.predicted_output_tokens = output_tokens;
    f.predicted_output_sigma = static_cast<float>(output_tokens) * 0.35f;
    f.role_hint = "planner";
    f.stream = true;
    return f;
}

const rf::Candidate* find(const rf::Decision& d, const std::string& node_id) {
    for (const auto& c : d.candidates)
        if (c.node_id == node_id) return &c;
    return nullptr;
}

// --- admission --------------------------------------------------------------

void test_admission() {
    section("admission (§6.1)");

    auto cost = rf::make_static_cost_model(bench_scoring());
    rf::EmptyLedger ledger;
    const rf::ScoringConfig scoring = bench_scoring();
    const auto cluster = make_cluster();
    const auto req = planner_request(140);

    const rf::AdmissionResult laptop =
        rf::admit(req, cluster[2], ledger, *cost, scoring, rf::now_ms(), {});
    check(!laptop.admitted && laptop.reason == rf::AdmitReason::InsufficientVram,
          "a 7 GB model does not fit a 6 GB card, even with everything evicted");

    const rf::AdmissionResult jetson =
        rf::admit(req, cluster[1], ledger, *cost, scoring, rf::now_ms(), {});
    check(jetson.admitted && jetson.would_evict.empty(),
          "a node already holding the model needs no room made");

    rf::NodeState dead = cluster[0];
    dead.engine_healthy = false;
    check(rf::admit(req, dead, ledger, *cost, scoring, rf::now_ms(), {}).reason ==
              rf::AdmitReason::EngineDown,
          "a dead engine is rejected as engine_down");

    rf::NodeState stale = cluster[0];
    stale.sampled_at_ms = rf::now_ms() - 60000;
    check(rf::admit(req, stale, ledger, *cost, scoring, rf::now_ms(), {}).reason ==
              rf::AdmitReason::NodeStale,
          "a node we have not heard from is rejected as node_stale");

    rf::NodeState no_model = cluster[0];
    no_model.models_on_disk.clear();
    no_model.model_disk_bytes.clear();
    check(rf::admit(req, no_model, ledger, *cost, scoring, rf::now_ms(), {}).reason ==
              rf::AdmitReason::ModelMissing,
          "a model that is not on disk is rejected as model_missing");

    check(rf::admit(req, cluster[0], ledger, *cost, scoring, rf::now_ms(),
                    {"sim-desktop"})
              .reason == rf::AdmitReason::Excluded,
          "an operator-excluded node is rejected as excluded");

    // D7: rev 1 rejected any node without enough *free* VRAM. A node holding an
    // idle model it could drop is a candidate, and the eviction gets priced.
    rf::NodeState full = make_node("full", "NVIDIA GeForce RTX 4060 Laptop GPU",
                                   8 * kGB, 1 * kGB);
    add_disk(full, "planner:12b", 6800 * 1000 * 1000ULL);
    add_resident(full, "worker:3b", 2 * kGB, rf::now_ms() - 5000);
    add_resident(full, "other:7b", 5 * kGB, rf::now_ms() - 600000);
    const rf::AdmissionResult evicting =
        rf::admit(req, full, ledger, *cost, scoring, rf::now_ms(), {});
    check(evicting.admitted, "a node that can evict to make room is admissible (D7)");
    check(!evicting.would_evict.empty(), "and it reports what it would evict");
    check(!evicting.would_evict.empty() && evicting.would_evict.front() == "other:7b",
          "evicting least-recently-used first");
}

// --- the central claim ------------------------------------------------------

void test_warmth_beats_idle_power() {
    section("warm weak node vs idle strong node (§1)");

    auto cost = rf::make_static_cost_model(bench_scoring());
    auto warmth = rf::make_warmth_policy();
    rf::EmptyLedger ledger;
    const rf::ScoringConfig scoring = bench_scoring();
    const auto cluster = make_cluster();

    // Short generation: the load dominates, so the warm slow node wins even
    // though it decodes at roughly twice the speed. This is the thesis.
    {
        const rf::Decision d =
            warmth->select(planner_request(40), cluster, ledger, *cost, scoring);
        check_eq(d.winner_node_id, "sim-jetson",
                 "short reply: the warm Jetson beats the idle desktop");
        check_eq(d.decided_by, "t_load",
                 "and the trace attributes it to t_load, not to a warmth bonus");

        const rf::Candidate* jetson = find(d, "sim-jetson");
        const rf::Candidate* desktop = find(d, "sim-desktop");
        check(jetson && jetson->est.t_load_ms == 0, "the warm node pays no load");
        check(desktop && desktop->est.t_load_ms > 5000,
              "the cold node pays seconds of load, as a measured number");
        check(desktop && desktop->est.t_decode_ms < jetson->est.t_decode_ms,
              "the desktop is genuinely the faster decoder");

        const rf::Candidate* laptop = find(d, "sim-laptop");
        check(laptop && !laptop->admitted, "the 6 GB node is not a candidate");
        check(laptop && laptop->vram_free_bytes > 0,
              "but its state is still recorded, for counterfactuals (D10)");
    }

    // Long generation: decode dominates, and paying the load on the fast node is
    // the right answer. A policy that always chose warmth would get this wrong.
    {
        const rf::Decision d =
            warmth->select(planner_request(1200), cluster, ledger, *cost, scoring);
        check_eq(d.winner_node_id, "sim-desktop",
                 "long reply: loading on the fast node beats staying warm on the slow one");
    }

    // In between, the two are genuinely indistinguishable given how uncertain
    // output length is, and the honest answer is to say so. This is not a
    // degraded case to be tuned away — a ±35% band on the largest term really
    // does swallow a 900 ms margin, and a scheduler that reported "t_load" here
    // would be claiming a precision it does not have (D8).
    {
        const rf::Decision d =
            warmth->select(planner_request(140), cluster, ledger, *cost, scoring);
        check_eq(d.decided_by, "within_noise",
                 "mid-length reply: the margin is inside sigma and is reported as noise");
        check(!d.winner_node_id.empty(),
              "a choice is still made — noise is not an excuse to refuse to route");
    }
}

// --- the ledger (D4) --------------------------------------------------------

void test_ledger_prevents_duplicate_loads() {
    section("ledger: herd and duplicate loads (§6.3, D4)");

    auto cost = rf::make_static_cost_model(bench_scoring());
    auto warmth = rf::make_warmth_policy();
    const rf::ScoringConfig scoring = bench_scoring();
    rf::NodeLedger ledger;

    // Two nodes, neither holding the model, identical hardware. The first
    // request picks one and reserves; the second must see that load in flight.
    rf::NodeState a = make_node("a", "NVIDIA GeForce RTX 4060 Laptop GPU", 16 * kGB,
                                16 * kGB);
    rf::NodeState b = make_node("b", "NVIDIA GeForce RTX 4060 Laptop GPU", 16 * kGB,
                                16 * kGB);
    add_disk(a, "planner:12b", 6800 * 1000 * 1000ULL);
    add_disk(b, "planner:12b", 6800 * 1000 * 1000ULL);
    const std::vector<rf::NodeState> pair = {a, b};

    const auto req = planner_request(140);
    const rf::Decision first = warmth->select(req, pair, ledger, *cost, scoring);
    check(!first.winner_node_id.empty(), "the first request picks a node");

    const rf::Candidate* winner = first.winner();
    const uint64_t footprint =
        cost->footprint_bytes(req.model, pair[0], req.num_ctx);
    const rf::NodeLedger::Token token =
        ledger.reserve(first.winner_node_id, req.model, footprint, true,
                       winner->est.t_load_ms, rf::now_ms());
    check(token != rf::NodeLedger::kInvalid, "and reserves it");
    check(ledger.pending(first.winner_node_id, req.model) != nullptr,
          "the load is now pending on that node");
    check(ledger.reserved_vram_bytes(first.winner_node_id) == footprint,
          "and its VRAM is reserved against a concurrent scorer");

    // The second request sees the in-flight load. At the instant of reservation
    // the remaining time equals a fresh load, so the two are priced the same —
    // correctly: with VRAM free on both nodes, loading in parallel really is
    // faster for the client than queueing behind one load and one generation.
    // What the ledger guarantees is that the choice is *priced*, not that it is
    // always made one way (see D21).
    const rf::Decision second = warmth->select(req, pair, ledger, *cost, scoring);
    const rf::Candidate* same = find(second, first.winner_node_id);
    const rf::Candidate* other =
        find(second, first.winner_node_id == "a" ? "b" : "a");
    check(same && other, "both nodes are still candidates");
    check(same && same->inflight == 1, "the ledger's in-flight count is visible");
    check(same && same->est.t_queue_ms > 0,
          "and the busy node is charged for its queue, so the herd is broken");
    check(other && other->est.t_queue_ms == 0, "while the idle node is not");

    // As the load progresses, waiting for it does become cheaper than starting
    // another. That is the case the pending-load path exists for.
    rf::NodeLedger aged;
    const double eta = winner->est.t_load_ms;
    aged.reserve(first.winner_node_id, req.model, footprint, true, eta,
                 rf::now_ms() - static_cast<int64_t>(eta * 0.8));
    const rf::Decision late = warmth->select(req, pair, aged, *cost, scoring);
    const rf::Candidate* nearly_loaded = find(late, first.winner_node_id);
    const rf::Candidate* fresh = find(late, first.winner_node_id == "a" ? "b" : "a");
    check(nearly_loaded && fresh &&
              nearly_loaded->est.t_load_ms < fresh->est.t_load_ms * 0.5,
          "a load that is 80% done is priced well below a fresh one");

    ledger.release(token);
    check(ledger.pending(first.winner_node_id, req.model) == nullptr,
          "releasing clears the pending load");
    check(ledger.reserved_vram_bytes(first.winner_node_id) == 0,
          "and the VRAM reservation");
    check(ledger.inflight(first.winner_node_id) == 0, "and the in-flight count");
}

// --- queueing and contention (D5, D6) ---------------------------------------

void test_queue_and_contention() {
    section("queueing and contention (§6.2, D5, D6)");

    auto cost = rf::make_static_cost_model(bench_scoring());
    const rf::ScoringConfig scoring = bench_scoring();
    const auto cluster = make_cluster();
    const auto req = planner_request(140);
    const rf::EvictionPlan none;

    rf::NodeLedger ledger;
    const rf::NodeState& jetson = cluster[1];

    const rf::Estimate idle =
        cost->estimate(req, jetson, ledger, none, rf::now_ms());
    check(idle.t_queue_ms == 0, "an idle node has no queue term");

    // One request in flight, one slot: the next one genuinely waits.
    const uint64_t footprint = cost->footprint_bytes(req.model, jetson, req.num_ctx);
    const rf::NodeLedger::Token first =
        ledger.reserve(jetson.id, req.model, footprint, false, 0, rf::now_ms());
    const rf::Estimate queued =
        cost->estimate(req, jetson, ledger, none, rf::now_ms());
    check(queued.t_queue_ms > 0, "a full node charges a queue term (D6)");
    check(std::fabs(queued.t_decode_ms - idle.t_decode_ms) < 1e-6,
          "queueing alone does not change the decode rate — contention is priced "
          "once, not twice");

    // Now the in-flight request is actually decoding, which is what slows the
    // GPU down for everyone.
    ledger.note_decoding(first);
    const rf::Estimate contended =
        cost->estimate(req, jetson, ledger, none, rf::now_ms());
    check(contended.t_decode_ms > idle.t_decode_ms * 1.9,
          "a concurrent decoder roughly halves throughput at alpha = 1 (D5)");

    ledger.release(first);
    const rf::Estimate after = cost->estimate(req, jetson, ledger, none, rf::now_ms());
    check(std::fabs(after.t_decode_ms - idle.t_decode_ms) < 1e-6,
          "and it recovers when the request completes");
}

// --- uncertainty and omitted terms (D8, D3) ---------------------------------

void test_uncertainty_and_omission() {
    section("uncertainty and omitted terms (§6.2, D8, D3)");

    auto cost = rf::make_static_cost_model(bench_scoring());
    auto warmth = rf::make_warmth_policy();
    const rf::ScoringConfig scoring = bench_scoring();
    rf::EmptyLedger ledger;

    // Two near-identical nodes: the margin is far inside the uncertainty, and
    // the scheduler has to say so rather than present a coin flip as a result.
    rf::NodeState a = make_node("a", "NVIDIA GeForce RTX 4060 Laptop GPU", 16 * kGB,
                                16 * kGB);
    rf::NodeState b = a;
    b.id = "b";
    add_disk(a, "worker:3b", 1900 * 1000 * 1000ULL);
    add_disk(b, "worker:3b", 1900 * 1000 * 1000ULL);
    add_resident(a, "worker:3b", 2 * kGB, rf::now_ms());
    add_resident(b, "worker:3b", 2 * kGB, rf::now_ms());

    rf::RequestFeatures req;
    req.model = "worker:3b";
    req.prompt_tokens = 400;
    req.predicted_output_tokens = 60;
    req.predicted_output_sigma = 21.f;

    const rf::Decision d = warmth->select(req, {a, b}, ledger, *cost, scoring);
    check(!d.winner_node_id.empty(), "a winner is still chosen");
    check_eq(d.decided_by, "within_noise",
             "but a margin inside sigma is reported as noise, not as a decision");

    // D3: an engine that cannot report residency must not have T_load guessed
    // for it. The term is omitted and flagged, all the way to the UI.
    rf::NodeState opaque = a;
    opaque.id = "opaque";
    opaque.residency_known = false;
    opaque.models_resident.clear();
    const rf::Decision blind = warmth->select(req, {opaque}, ledger, *cost, scoring);
    const rf::Candidate* c = find(blind, "opaque");
    check(c && (c->est.omitted_terms & rf::kTermLoad),
          "an engine with unknown residency omits t_load rather than assuming cold");
    const std::vector<std::string> names = rf::term_flag_names(c->est.omitted_terms);
    check(!names.empty() && names.front() == "t_load",
          "and the omission is named in the record");
}

// --- the baseline ------------------------------------------------------------

void test_round_robin_is_a_baseline() {
    section("round robin (§9)");

    auto cost = rf::make_static_cost_model(bench_scoring());
    auto rr = rf::make_round_robin_policy();
    rf::EmptyLedger ledger;
    const rf::ScoringConfig scoring = bench_scoring();
    const auto cluster = make_cluster();
    const auto req = planner_request(140);

    std::vector<std::string> picks;
    for (int i = 0; i < 6; ++i)
        picks.push_back(rr->select(req, cluster, ledger, *cost, scoring).winner_node_id);

    bool rotated = false;
    for (size_t i = 1; i < picks.size(); ++i)
        if (picks[i] != picks[0]) rotated = true;
    check(rotated, "it rotates rather than pinning one node");
    for (const auto& p : picks)
        check(p != "sim-laptop", "it never picks an inadmissible node");

    const rf::Decision d = rr->select(req, cluster, ledger, *cost, scoring);
    check_eq(d.decided_by, "round_robin",
             "and it says it did not consult a term, rather than claiming one");
    const rf::Candidate* jetson = find(d, "sim-jetson");
    check(jetson && jetson->est.total_ms() > 0,
          "the baseline still records full predictions, so error is comparable");
}


// --- the learned cost model (Phase 2) ---------------------------------------

namespace {

// A completed job, shaped so the learned model can take it apart. The numbers
// are chosen so the intended rate is exact arithmetic, not something to eyeball.
rf::TraceRecord make_record(const std::string& node, const std::string& model,
                            bool warm, uint32_t prompt, uint32_t output,
                            double prefill_ms, double decode_ms,
                            uint32_t decoders, double load_ms = 0) {
    rf::TraceRecord r;
    r.job_id = rf::ulid();
    r.node_id = node;
    r.model = model;
    r.outcome = rf::Outcome::Ok;
    r.was_resident = warm;
    r.footprint_bytes = 7ULL * 1000 * 1000 * 1000;
    r.has_prompt_tokens_actual = true;
    r.prompt_tokens_actual = prompt;
    r.has_output_tokens = true;
    r.output_tokens = output;
    r.queue_wait_ms = 0;
    r.has_load_ms = warm || load_ms > 0;
    r.load_ms = warm ? 0 : load_ms;
    r.has_ttft = true;
    r.ttft_ms = r.load_ms + prefill_ms;
    r.total_ms = r.ttft_ms + decode_ms;
    r.inflight_at_dispatch = 0;
    r.concurrent_decoders_at_dispatch = decoders;
    return r;
}

}  // namespace

void test_learned_cost_model() {
    section("learned cost model (§8, Phase 2)");

    const rf::ScoringConfig scoring = bench_scoring();
    const auto cluster = make_cluster();
    const rf::NodeState& jetson = cluster[1];   // holds planner:12b warm
    rf::EmptyLedger ledger;
    const rf::EvictionPlan none;
    const auto req = planner_request(140);

    auto learned = rf::make_learned_cost_model(scoring);
    auto static_model = rf::make_static_cost_model(scoring);

    // Before any evidence the two must agree: §9 seeds the learned model from
    // the static table, so a fresh cluster is not worse off for using it.
    const rf::Estimate cold_start =
        learned->estimate(req, jetson, ledger, none, rf::now_ms());
    const rf::Estimate seeded =
        static_model->estimate(req, jetson, ledger, none, rf::now_ms());
    check(std::fabs(cold_start.total_ms() - seeded.total_ms()) < 1e-6,
          "with no observations the learned model matches its seed exactly");
    check(cold_start.conf == rf::Confidence::Seeded, "and reports itself as seeded");

    // Feed uncontended warm jobs whose real decode rate is 0.100 tok/ms — far
    // from the ~0.0129 the seed table predicts for this node and model.
    for (int i = 0; i < 30; ++i) {
        learned->observe(make_record("sim-jetson", "planner:12b", true,
                                     1200, 200, /*prefill_ms=*/400,
                                     /*decode_ms=*/2000, /*decoders=*/1));
    }
    const rf::Estimate taught =
        learned->estimate(req, jetson, ledger, none, rf::now_ms());
    check(taught.t_decode_ms < seeded.t_decode_ms * 0.5,
          "a measured decode rate replaces the seed");
    // 140 tokens at 0.100 tok/ms is 1400 ms.
    check(std::fabs(taught.t_decode_ms - 1400.0) < 60.0,
          "and lands on the observed rate, not somewhere between");
    check(taught.conf == rf::Confidence::Converged,
          "30 samples is reported as converged");

    // 1200 prompt tokens in 400 ms is 3.0 tok/ms.
    check(std::fabs(taught.t_prefill_ms - 400.0) < 20.0,
          "prefill is learned from the same warm records");

    // Contended records must not move the base rate — that is D5's whole point.
    const double before_contended = taught.t_decode_ms;
    for (int i = 0; i < 20; ++i) {
        // Two decoders, and the run really was half speed: alpha = 1.
        learned->observe(make_record("sim-jetson", "planner:12b", true,
                                     1200, 200, 400, 4000, /*decoders=*/2));
    }
    const rf::Estimate after_contended =
        learned->estimate(req, jetson, ledger, none, rf::now_ms());
    check(std::fabs(after_contended.t_decode_ms - before_contended) < 1.0,
          "contended jobs do not corrupt the uncontended decode rate (D5)");

    // They teach alpha instead, which shows up only when something else is
    // decoding on that node.
    struct OneDecoder : rf::EmptyLedger {
        uint32_t concurrent_decoders(const std::string&) const override { return 1; }
    } busy;
    const rf::Estimate contended =
        learned->estimate(req, jetson, busy, none, rf::now_ms());
    check(contended.t_decode_ms > after_contended.t_decode_ms * 1.8,
          "a second decoder roughly halves throughput, as the records showed");

    // Load bandwidth comes from cold starts. The record must carry the same
    // footprint the estimator will divide by, or the two disagree by the safety
    // margin and the KV term and the arithmetic stops being checkable.
    const rf::NodeState& desktop = cluster[0];   // nothing resident
    const uint64_t desktop_footprint =
        learned->footprint_bytes("planner:12b", desktop, req.num_ctx);
    const rf::Estimate before_load =
        learned->estimate(req, desktop, ledger, none, rf::now_ms());
    for (int i = 0; i < 10; ++i) {
        rf::TraceRecord cold = make_record("sim-desktop", "planner:12b", false,
                                           1200, 200, 400, 2000, 1,
                                           /*load_ms=*/7000);
        cold.footprint_bytes = desktop_footprint;
        learned->observe(cold);
    }
    const rf::Estimate after_load =
        learned->estimate(req, desktop, ledger, none, rf::now_ms());
    check(std::fabs(after_load.t_load_ms - 7000.0) < 400.0,
          "load bandwidth is learned from footprint over measured load time");
    check(after_load.sigma_ms < before_load.sigma_ms,
          "and the cold candidate stops carrying a band it did not earn (D8)");

    // Output length: with no cap from the caller, the model should predict what
    // it has seen rather than the blind seed, and its sigma should be the
    // measured error rather than a guessed fraction (§8).
    rf::RequestFeatures uncapped = req;
    uncapped.predicted_output_tokens = 0;
    uncapped.predicted_output_sigma = 0;
    const rf::OutputPrediction blind = static_model->predict_output(uncapped);
    const rf::OutputPrediction informed = learned->predict_output(uncapped);
    check(informed.tokens != blind.tokens, "output length is learned, not seeded");
    check(std::abs(static_cast<int>(informed.tokens) - 200) < 25,
          "and matches the lengths actually generated");
    check(informed.sigma < blind.sigma,
          "with a narrower band than the blind prior, which is what turns "
          "within_noise decisions into real ones");
    check(informed.sigma > 0,
          "but never zero — a perfect run does not make the next one certain");

    // A failed job describes a broken transfer, not a node's speed.
    auto fresh = rf::make_learned_cost_model(scoring);
    for (int i = 0; i < 30; ++i) {
        rf::TraceRecord bad = make_record("sim-jetson", "planner:12b", true,
                                          1200, 200, 400, 2000, 1);
        bad.outcome = rf::Outcome::StreamFailed;
        fresh->observe(bad);
    }
    const rf::Estimate unlearned =
        fresh->estimate(req, jetson, ledger, none, rf::now_ms());
    check(std::fabs(unlearned.total_ms() - seeded.total_ms()) < 1e-6,
          "failed jobs teach nothing");
}
}  // namespace

int main() {
    rf::log_set_level(rf::LogLevel::Error);

    test_admission();
    test_warmth_beats_idle_power();
    test_ledger_prevents_duplicate_loads();
    test_queue_and_contention();
    test_uncertainty_and_omission();
    test_round_robin_is_a_baseline();
    test_learned_cost_model();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
