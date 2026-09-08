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
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include "common/types.h"
#include "common/util.h"
#include "agent/engine/residency_limit.h"
#include "router/core/admission.h"
#include "router/core/trace_writer.h"
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

// The window between a load finishing and the next poll observing it. Found on
// real hardware: the card had just spent 75 s loading a 4.9 GB model, NVML
// reported the VRAM gone, and Ollama's residency list had not caught up. Three
// requests for that exact model were refused for want of VRAM the model itself
// was occupying (D36).
void test_admission_trusts_its_own_completion() {
    section("admission in the window after a load (D36)");

    const rf::ScoringConfig scoring = bench_scoring();
    auto cost = rf::make_static_cost_model(scoring);
    const auto req = planner_request(140);

    // A node whose telemetry has seen the load but whose residency list has
    // not: free VRAM is gone, models_resident is empty.
    rf::NodeState node = make_cluster()[0];
    node.models_resident.clear();
    node.vram_free_bytes = 500ULL * 1000 * 1000;   // the model is in there
    node.sampled_at_ms = rf::now_ms() - 1000;

    struct JustServed : rf::EmptyLedger {
        int64_t when = 0;
        int64_t last_served_ms(const std::string&, const std::string&) const override {
            return when;
        }
    };

    const int64_t now = rf::now_ms();

    JustServed stale;
    stale.when = now - scoring.node_stale_ms - 1000;   // beyond the horizon
    const rf::AdmissionResult refused =
        rf::admit(req, node, stale, *cost, scoring, now, {});
    check(!refused.admitted && refused.reason == rf::AdmitReason::InsufficientVram,
          "a completion older than the staleness horizon does not override VRAM");

    JustServed fresh;
    fresh.when = now - 100;                            // just served
    const rf::AdmissionResult admitted =
        rf::admit(req, node, fresh, *cost, scoring, now, {});
    check(admitted.admitted,
          "having just served it here is first-hand evidence of residency");

    // Never served at all is the ordinary cold case and must stay refused, or
    // the rule would admit every node that happens to be full.
    JustServed never;
    const rf::AdmissionResult cold =
        rf::admit(req, node, never, *cost, scoring, now, {});
    check(!cold.admitted,
          "a node we have never served this model on is still judged on VRAM");
}

// An engine that keeps a fixed number of models resident (D38). None report
// the setting, so the agent infers it and the router has to act on it in three
// places: what it believes is loaded, what it plans to evict, and whether a
// preload is a preload at all.
void test_residency_limit() {
    section("an engine's resident-model limit (D38)");

    // --- the agent's inference ------------------------------------------
    //
    // Driven with the transitions a real Ollama actually produces, measured at
    // 2 Hz: it drops first and loads second, so the resident set is empty in
    // between. An earlier version of this looked for a gain and a loss in the
    // same poll and would never have fired once.
    {
        const int64_t t0 = 1000000;
        const uint64_t roomy = 7ULL * 1000 * 1000 * 1000;
        rf::ResidencyLimit lim;

        auto poll = [&lim](std::vector<std::pair<std::string, int64_t>> models,
                           uint64_t free, int64_t at) {
            std::vector<rf::ResidentModel> r;
            for (const auto& nm : models) {
                rf::ResidentModel m;
                m.name = nm.first;
                m.vram_bytes = 400ULL * 1000 * 1000;
                m.expires_at_ms = nm.second;
                r.push_back(m);
            }
            return lim.observe(r, true, free, at);
        };

        // A holds the engine, expiring five minutes out. Then nothing. Then B.
        poll({{"a", t0 + 300000}}, roomy, t0);
        poll({}, roomy, t0 + 1000);                       // mid-swap
        check(poll({{"b", t0 + 302000}}, roomy, t0 + 2000) == 0,
              "one early drop is a coincidence until it repeats");
        poll({}, roomy, t0 + 3000);
        check(poll({{"a", t0 + 304000}}, roomy, t0 + 4000) == 1,
              "a model dropped before its own expiry, twice, with room to "
              "spare, is the engine showing its ceiling");

        check(poll({{"a", t0 + 304000}, {"b", t0 + 305000}}, roomy, t0 + 5000) == 0,
              "and seeing more models resident than that withdraws it");
    }
    {
        // A model that simply timed out says nothing about capacity. Same
        // shape, but each departure happens after its own expiry.
        const int64_t t0 = 1000000;
        rf::ResidencyLimit lim;
        auto poll = [&lim](std::vector<std::pair<std::string, int64_t>> models,
                           int64_t at) {
            std::vector<rf::ResidentModel> r;
            for (const auto& nm : models) {
                rf::ResidentModel m;
                m.name = nm.first;
                m.vram_bytes = 400ULL * 1000 * 1000;
                m.expires_at_ms = nm.second;
                r.push_back(m);
            }
            return lim.observe(r, true, 7ULL * 1000 * 1000 * 1000, at);
        };
        poll({{"a", t0 + 500}}, t0);        // due to expire almost immediately
        poll({}, t0 + 1000);
        poll({{"b", t0 + 1500}}, t0 + 2000);
        poll({}, t0 + 3000);
        poll({{"a", t0 + 3500}}, t0 + 4000);
        check(lim.value() == 0,
              "a model that left after its expiry was idle, not displaced");
    }
    {
        // Swapping because there was no room is ordinary VRAM pressure.
        const int64_t t0 = 1000000;
        rf::ResidencyLimit lim;
        auto tight = [&lim](std::vector<std::string> names, int64_t at) {
            std::vector<rf::ResidentModel> r;
            for (const auto& n : names) {
                rf::ResidentModel m;
                m.name = n;
                m.vram_bytes = 6ULL * 1000 * 1000 * 1000;
                m.expires_at_ms = at + 300000;
                r.push_back(m);
            }
            return lim.observe(r, true, 1ULL * 1000 * 1000 * 1000, at);
        };
        tight({"a"}, t0);
        tight({}, t0 + 1000);
        tight({"b"}, t0 + 2000);
        tight({}, t0 + 3000);
        tight({"a"}, t0 + 4000);
        check(lim.value() == 0,
              "a swap that memory pressure explains is not evidence of a "
              "ceiling -- which is also why a simulated node, whose capacity "
              "is its VRAM, can never invent one");
    }
    {
        // Far enough apart and the two observations are not one event.
        const int64_t t0 = 1000000;
        rf::ResidencyLimit lim;
        auto poll = [&lim](std::vector<std::pair<std::string, int64_t>> models,
                           int64_t at) {
            std::vector<rf::ResidentModel> r;
            for (const auto& nm : models) {
                rf::ResidentModel m;
                m.name = nm.first;
                m.vram_bytes = 400ULL * 1000 * 1000;
                m.expires_at_ms = nm.second;
                r.push_back(m);
            }
            return lim.observe(r, true, 7ULL * 1000 * 1000 * 1000, at);
        };
        poll({{"a", t0 + 300000}}, t0);
        poll({{"b", t0 + 400000}}, t0 + 120000);
        poll({{"a", t0 + 500000}}, t0 + 240000);
        check(lim.value() == 0,
              "two minutes apart, a departure and an arrival are not one swap");
    }
    {
        rf::ResidencyLimit lim;
        std::vector<rf::ResidentModel> none;
        check(lim.observe(none, false, 0, 1000) == 0,
              "an engine that will not say what is resident teaches nothing");
    }
    {
        // An engine that reports no expiry is not saying it dropped anything
        // early, so it is not saying anything at all.
        const int64_t t0 = 1000000;
        rf::ResidencyLimit lim;
        auto poll = [&lim](std::vector<std::string> names, int64_t at) {
            std::vector<rf::ResidentModel> r;
            for (const auto& n : names) {
                rf::ResidentModel m;
                m.name = n;
                m.vram_bytes = 400ULL * 1000 * 1000;
                m.expires_at_ms = 0;   // engine does not say
                r.push_back(m);
            }
            return lim.observe(r, true, 7ULL * 1000 * 1000 * 1000, at);
        };
        poll({"a"}, t0);
        poll({}, t0 + 1000);
        poll({"b"}, t0 + 2000);
        poll({}, t0 + 3000);
        poll({"a"}, t0 + 4000);
        check(lim.value() == 0,
              "without an expiry to compare against, an absence proves nothing");
    }

    // --- what the router believes is loaded ------------------------------
    const rf::ScoringConfig scoring = bench_scoring();
    auto cost = rf::make_static_cost_model(scoring);
    const auto req = planner_request(140);
    const int64_t now = rf::now_ms();

    // The shape that produced 7 of 8 mispriced cold starts on real hardware:
    // we served this model here seconds ago, so D36's rule says warm -- but the
    // engine holds one model and has served another since.
    rf::NodeState node = make_cluster()[0];
    node.models_resident.clear();
    add_resident(node, "worker:3b", 2 * kGB, now - 500);
    node.vram_free_bytes = 6ULL * 1000 * 1000 * 1000;   // plenty
    node.sampled_at_ms = now - 100;

    struct Served : rf::EmptyLedger {
        int64_t ours = 0;
        uint32_t others = 0;
        int64_t last_served_ms(const std::string&, const std::string&) const override {
            return ours;
        }
        uint32_t models_served_since(const std::string&, const std::string&,
                                     int64_t) const override {
            return others;
        }
    };

    Served just_us;
    just_us.ours = now - 500;
    just_us.others = 0;

    node.models_resident_limit = 0;
    check(rf::believed_loaded(node, req.model, just_us, scoring, now),
          "with no known ceiling, a recent completion still means warm (D36)");

    node.models_resident_limit = 1;
    check(rf::believed_loaded(node, req.model, just_us, scoring, now),
          "a ceiling alone does not withdraw that: nothing has displaced us yet");

    Served displaced;
    displaced.ours = now - 500;
    displaced.others = 1;
    check(!rf::believed_loaded(node, req.model, displaced, scoring, now),
          "but once the engine has served its whole ceiling in other models, "
          "ours cannot still be there");

    // --- what admission plans to evict -----------------------------------
    rf::NodeState roomy = make_cluster()[1];       // sim-jetson, holds planner
    roomy.models_resident.clear();
    add_resident(roomy, "worker:3b", 2 * kGB, now - 60000);
    roomy.vram_free_bytes = 12ULL * 1000 * 1000 * 1000;  // room for both

    rf::EmptyLedger idle;
    roomy.models_resident_limit = 0;
    const rf::AdmissionResult free_ride =
        rf::admit(req, roomy, idle, *cost, scoring, now, {});
    check(free_ride.admitted && free_ride.would_evict.empty(),
          "with bytes to spare and no known ceiling, loading evicts nothing");

    roomy.models_resident_limit = 1;
    const rf::AdmissionResult swap =
        rf::admit(req, roomy, idle, *cost, scoring, now, {});
    check(swap.admitted, "a crowded engine can still take the request");
    check(swap.would_evict.size() == 1 && swap.would_evict[0] == "worker:3b",
          "but the displacement is planned and named, not discovered afterwards");
    check(swap.evict_bytes > 0,
          "so T_evict prices it like any other eviction (D7)");

    // Nothing droppable: every resident model is busy serving this router.
    struct AllBusy : rf::EmptyLedger {
        bool model_busy(const std::string&, const std::string&) const override {
            return true;
        }
    } busy;
    const rf::AdmissionResult stuck =
        rf::admit(req, roomy, busy, *cost, scoring, now, {});
    check(!stuck.admitted && stuck.reason == rf::AdmitReason::InsufficientVram,
          "and a full engine with nothing droppable is refused rather than "
          "sent a request it would have to interrupt");

    // --- pricing that eviction ------------------------------------------
    //
    // A real engine does not say when a model was last used, only when it
    // expires, so `last_used_ms` arrives as 0 and `evicts_warm` was never true
    // on hardware -- T_evict was live in simulation and dead in the field. The
    // router knows the answer from its own dispatches.
    // A node shaped the way a real Ollama reports one: resident, but with no
    // last-use, because the engine only publishes an expiry.
    rf::NodeState silent = roomy;
    silent.models_resident.clear();
    add_resident(silent, "worker:3b", 2 * kGB, 0);
    silent.models_resident_limit = 1;

    const rf::AdmissionResult unpriced =
        rf::admit(req, silent, idle, *cost, scoring, now, {});
    check(!unpriced.would_evict.empty(),
          "the displacement is still planned when the engine reports no "
          "last-use");
    check(!unpriced.evicts_warm,
          "but with nothing to date it by, it looks cold and T_evict stays "
          "zero -- which is what every real decision measured so far did");

    struct ServedVictim : rf::EmptyLedger {
        int64_t when = 0;
        int64_t last_served_ms(const std::string&, const std::string& model) const override {
            return model == "worker:3b" ? when : 0;
        }
    };
    ServedVictim recent;
    recent.when = now - 5000;                       // inside warm_window
    const rf::AdmissionResult warm_victim =
        rf::admit(req, silent, recent, *cost, scoring, now, {});
    check(warm_victim.evicts_warm,
          "the router's own record of serving it there dates it, and T_evict "
          "prices it (D7, D38)");

    ServedVictim stale_victim;
    stale_victim.when = now - scoring.warm_window_ms - 1000;
    const rf::AdmissionResult cold_victim =
        rf::admit(req, silent, stale_victim, *cost, scoring, now, {});
    check(!cold_victim.evicts_warm,
          "and a victim nobody has wanted for longer than the warm window is "
          "not charged for");
}

// Exclusion has to survive being right (D42). The list carries two things the
// router cannot afford to get wrong: the operator's own "do not use this node"
// (§6.1) and the nodes a request has already failed on (§6.4, D9). Both were
// applied after selection, and skipped whenever the excluded node had won --
// which is exactly when they matter.
void test_exclusion_binds_even_when_the_node_would_win() {
    section("an excluded node stays excluded when it is the best one (D42)");

    const rf::ScoringConfig scoring = bench_scoring();
    auto cost = rf::make_static_cost_model(scoring);
    auto warmth = rf::make_warmth_policy();
    auto rr = rf::make_round_robin_policy();
    rf::EmptyLedger ledger;
    const auto cluster = make_cluster();
    const auto req = planner_request(140);

    // sim-jetson holds planner:12b warm, so it wins on merit.
    const rf::Decision open =
        warmth->select(req, cluster, ledger, *cost, scoring, {});
    check(open.winner_node_id == "sim-jetson",
          "the warm node wins when nothing is excluded");

    const rf::Decision closed =
        warmth->select(req, cluster, ledger, *cost, scoring, {"sim-jetson"});
    check(closed.winner_node_id != "sim-jetson",
          "and loses when it is excluded, rather than winning anyway");
    check(!closed.winner_node_id.empty(),
          "the request still goes somewhere: exclusion narrows the field, it "
          "does not empty it");

    const rf::Candidate* c = find(closed, "sim-jetson");
    check(c && !c->admitted && c->reason == rf::AdmitReason::Excluded,
          "and it appears in the breakdown as rejected, with the reason (§10)");

    // The same for round robin, because admission is shared and a baseline that
    // ignores exclusions is not comparable to a policy that honours them.
    const rf::Decision rr_closed =
        rr->select(req, cluster, ledger, *cost, scoring, {"sim-jetson"});
    check(rr_closed.winner_node_id != "sim-jetson",
          "round robin honours it too, or the two arms are not filtered alike");

    // Exclude everything and the answer is "nowhere", not "the excluded one".
    const rf::Decision none =
        warmth->select(req, cluster, ledger, *cost, scoring,
                       {"sim-desktop", "sim-jetson", "sim-laptop"});
    check(none.winner_node_id.empty(),
          "excluding every node leaves no winner, rather than quietly picking "
          "one of them");
}

// The trace is append-only and unbounded, which is right for a benchmark run
// and wrong for a router that stays up for months: the file grows forever and
// every restart replays all of it (D43).
void test_trace_rotation_and_tail() {
    section("trace rotation and bounded replay (D43)");

    const std::string path = "rf_rotate_test.jsonl";
    const std::string prev = path + ".1";
    std::remove(path.c_str());
    std::remove(prev.c_str());

    auto record = [](int i) {
        rf::TraceRecord r;
        r.job_id = "job-" + std::to_string(i);
        r.model = "m";
        r.node_id = "n";
        r.outcome = rf::Outcome::Ok;
        r.total_ms = i;
        return r;
    };

    // One record, to learn how big one is, so the budget below is expressed in
    // records rather than in a guess about bytes.
    {
        rf::TraceWriter w;
        std::string err;
        check(w.open(path, &err, 0), "a writer opens with rotation disabled");
        w.append(record(0));
    }
    std::ifstream sizer(path, std::ios::binary | std::ios::ate);
    const uint64_t one = static_cast<uint64_t>(sizer.tellg());
    sizer.close();
    check(one > 0, "and writes something");
    std::remove(path.c_str());

    {
        rf::TraceWriter w;
        std::string err;
        // Room for about ten records before the eleventh has to rotate.
        check(w.open(path, &err, one * 10), "a writer opens with a byte budget");
        for (int i = 0; i < 25; ++i) w.append(record(i));
        check(w.rotations() >= 1, "which rotates once the budget is reached");
        check(w.records_written() == 25,
              "and no record is dropped in the process");
    }

    std::ifstream live(path, std::ios::binary | std::ios::ate);
    check(static_cast<uint64_t>(live.tellg()) <= one * 10,
          "the live file stays inside its budget");
    live.close();

    {
        std::ifstream old(prev, std::ios::binary);
        check(old.good(), "and the previous generation is kept, not deleted");
    }

    // Every surviving record still parses, in both files: rotation must not
    // corrupt the seam or leave a half-written line.
    //
    // It does lose the oldest records, and that is the whole point rather than
    // a defect: twenty-five records through a ten-record budget rotates twice,
    // and the second rotation replaces the first `.1`. Bounded disk means
    // discarding something, and what it discards is the half an EWMA with a
    // twenty-sample half-life has already forgotten.
    uint64_t total = 0;
    double oldest = 1e9, newest = -1;
    uint64_t parse_errors = 0;
    std::string err;
    for (const std::string& f : {prev, path}) {
        rf::TraceReadStats one_file;
        rf::read_trace(f, [&](const rf::TraceRecord& r) {
            ++total;
            oldest = std::min(oldest, r.total_ms);
            newest = std::max(newest, r.total_ms);
        }, &one_file, &err);
        parse_errors += one_file.parse_errors;
    }
    check(parse_errors == 0, "no record is split across the rotation");
    check(newest == 24, "the newest record always survives");
    check(oldest > 0, "and the oldest is the one dropped, not the newest");
    check(total > 0 && total < 25,
          "so a bounded trace holds a bounded window, not everything");

    // The tail read: fewer records, no parse errors from the seam, and the
    // *last* ones rather than the first.
    rf::TraceReadStats full, tail;
    std::vector<double> tail_ids;
    rf::read_trace(path, nullptr, &full, &err, 0);
    rf::read_trace(path,
                   [&tail_ids](const rf::TraceRecord& r) { tail_ids.push_back(r.total_ms); },
                   &tail, &err, one * 3);
    check(tail.parsed > 0 && tail.parsed < full.parsed,
          "a tail read returns some records but not all of them");
    check(tail.parse_errors == 0,
          "and the partial line at the seam is dropped, not counted as corrupt");
    check(!tail_ids.empty() && tail_ids.back() == 24,
          "the tail is the newest records, which is the only half worth "
          "replaying into an EWMA");

    // A budget larger than the file is not an error and must not truncate.
    rf::TraceReadStats big;
    rf::read_trace(path, nullptr, &big, &err, one * 10000);
    check(big.parsed == full.parsed,
          "a tail larger than the file reads the whole file");

    std::remove(path.c_str());
    std::remove(prev.c_str());
}

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
            warmth->select(planner_request(40), cluster, ledger, *cost, scoring, {});
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
            warmth->select(planner_request(1200), cluster, ledger, *cost, scoring, {});
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
            warmth->select(planner_request(140), cluster, ledger, *cost, scoring, {});
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
    const rf::Decision first = warmth->select(req, pair, ledger, *cost, scoring, {});
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
    const rf::Decision second = warmth->select(req, pair, ledger, *cost, scoring, {});
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
    const rf::Decision late = warmth->select(req, pair, aged, *cost, scoring, {});
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

    const rf::Decision d = warmth->select(req, {a, b}, ledger, *cost, scoring, {});
    check(!d.winner_node_id.empty(), "a winner is still chosen");
    check_eq(d.decided_by, "within_noise",
             "but a margin inside sigma is reported as noise, not as a decision");

    // D3: an engine that cannot report residency must not have T_load guessed
    // for it. The term is omitted and flagged, all the way to the UI.
    rf::NodeState opaque = a;
    opaque.id = "opaque";
    opaque.residency_known = false;
    opaque.models_resident.clear();
    const rf::Decision blind = warmth->select(req, {opaque}, ledger, *cost, scoring, {});
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
        picks.push_back(rr->select(req, cluster, ledger, *cost, scoring, {}).winner_node_id);

    bool rotated = false;
    for (size_t i = 1; i < picks.size(); ++i)
        if (picks[i] != picks[0]) rotated = true;
    check(rotated, "it rotates rather than pinning one node");
    for (const auto& p : picks)
        check(p != "sim-laptop", "it never picks an inadmissible node");

    const rf::Decision d = rr->select(req, cluster, ledger, *cost, scoring, {});
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

// Contention is counted when decoding starts, not when the request is sent
// (D44). A burst of requests is dispatched before any of them has produced a
// token, so the dispatch sample reads zero for every one of them however hard
// they then contend -- and each contended run is then taught to the model as
// an uncontended one.
void test_contention_counted_at_first_token() {
    section("contention is counted when decoding begins (D44)");

    {
        rf::NodeLedger ledger;
        const int64_t now = rf::now_ms();
        const rf::NodeLedger::Token a = ledger.reserve("n", "m", 0, false, 0, now);
        const rf::NodeLedger::Token b = ledger.reserve("n", "m", 0, false, 0, now);
        const rf::NodeLedger::Token c = ledger.reserve("n", "m", 0, false, 0, now);
        check(a != rf::NodeLedger::kInvalid, "the ledger admits three requests");
        check(ledger.note_decoding(a) == 1,
              "the first to produce a token is alone");
        check(ledger.note_decoding(b) == 2,
              "the second sees the first, at the moment it can");
        check(ledger.note_decoding(c) == 3, "and the third sees both");
        check(ledger.note_decoding(b) == 3,
              "a repeated first token does not double-count the same request");
        ledger.release(a);
        check(ledger.note_decoding(c) == 2,
              "and the count follows the ledger as requests finish");
    }

    // What the cost model does with it: a contended record must teach alpha,
    // never the base decode rate.
    const rf::ScoringConfig scoring = bench_scoring();
    const auto cluster = make_cluster();
    const rf::NodeState& jetson = cluster[1];
    rf::EmptyLedger ledger;
    const rf::EvictionPlan none;
    const auto req = planner_request(200);

    auto rate_after = [&](uint32_t at_dispatch, uint32_t at_first_token) {
        auto m = rf::make_learned_cost_model(scoring);
        for (int i = 0; i < 30; ++i) {
            // 200 tokens in 2000 ms: 0.1 tok/ms, half the fixture's warm rate.
            rf::TraceRecord r = make_record("sim-jetson", "planner:12b", true,
                                            1200, 200, 400, 2000, at_dispatch);
            r.concurrent_decoders_at_first_token = at_first_token;
            m->observe(r);
        }
        return m->estimate(req, jetson, ledger, none, rf::now_ms()).t_decode_ms;
    };

    // The shape a burst produces today: dispatch says nobody, first token says
    // four. Against the shape where the router had been told at dispatch.
    const double blind = rate_after(0, 0);
    const double honest = rate_after(0, 4);
    const double told_at_dispatch = rate_after(4, 0);

    check(std::fabs(honest - told_at_dispatch) < honest * 0.05,
          "a run marked contended at the first token is treated exactly like "
          "one that was contended at dispatch");
    // 200 tokens over 2000 ms is 0.1 tok/ms. Unnoticed, that becomes the base
    // rate, and the model then predicts precisely the contended speed it was
    // shown as though it were the uncontended one -- which is the defect,
    // stated as a number rather than as a worry.
    check(std::fabs(blind - 2000.0) < 200.0,
          "unnoticed contention is adopted as the uncontended decode rate");
    check(std::fabs(honest - blind) > blind * 0.5,
          "and noticing it keeps that sample out of the base rate entirely");
}

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

    // The estimate has to carry the prediction it used, because that is what
    // the trace records. Reading the caller's cap instead writes a zero on
    // every uncapped request, and a zero is indistinguishable from "the model
    // said nothing" — which makes the length error unmeasurable and leaves the
    // learned sigma permanently guessed (D30).
    const rf::Estimate uncapped_est =
        learned->estimate(uncapped, jetson, ledger, none, rf::now_ms());
    check(uncapped_est.predicted_output_tokens == informed.tokens,
          "the estimate reports the output length it actually priced");
    check(uncapped_est.predicted_output_sigma == informed.sigma,
          "and the sigma that went with it");
    const rf::Estimate seed_est =
        static_model->estimate(uncapped, jetson, ledger, none, rf::now_ms());
    check(seed_est.predicted_output_tokens == blind.tokens,
          "and the static model reports its blind prior rather than nothing");

    // Sigma is the model's own residual. A replayed trace was written by
    // whichever model was running at the time — the warm-up campaign runs the
    // static one — so reading the record's prediction would make the learned
    // model adopt that model's error and hand out a band several times too
    // wide, on data it actually fits well (D31).
    auto replayed = rf::make_learned_cost_model(scoring);
    for (int i = 0; i < 30; ++i) {
        rf::TraceRecord r = make_record("sim-jetson", "planner:12b", true,
                                        1200, 200, 400, 2000, 1);
        r.role_hint = "planner";
        r.predicted_output_tokens = 1000;   // what the *other* model guessed
        r.predicted_output_sigma = 800.f;
        replayed->observe(r);
    }
    const rf::OutputPrediction inherited = replayed->predict_output(uncapped);
    check(std::abs(static_cast<int>(inherited.tokens) - 200) < 25,
          "a trace written by another model still teaches the right length");
    check(inherited.sigma < 40.f,
          "and its error is not adopted as our own uncertainty");

    // The band has a floor, and the floor is measured (D39). Sigma was built
    // from output-length error and a guessed fraction of the load time: no
    // queue term, no prefill term, nothing for a rate being wrong. On real
    // hardware it covered 7-14% of outcomes where a 1-sigma band covers 68%.
    {
        auto band = rf::make_learned_cost_model(scoring);
        rf::NodeState node = cluster[1];

        const rf::Estimate before =
            band->estimate(req, node, ledger, none, rf::now_ms());

        // Records this model produced, whose outcome missed the prediction by
        // a wide and consistent margin. Nothing here teaches a rate: the
        // prediction is simply wrong, which is the case sigma exists for.
        for (int i = 0; i < 20; ++i) {
            rf::TraceRecord r = make_record("sim-jetson", "planner:12b", true,
                                            1200, 200, 400, 2000, 1);
            r.cost_model = band->name();
            r.predicted_total_ms = 2400;
            r.total_ms = 12400;             // out by ten seconds, every time
            rf::Candidate c;
            c.node_id = "sim-jetson";
            c.admitted = true;              // warm regime: no load, no queue
            r.candidates.push_back(c);
            band->observe(r);
        }
        const rf::Estimate after =
            band->estimate(req, node, ledger, none, rf::now_ms());
        check(after.sigma_ms > before.sigma_ms * 3,
              "a node that has been consistently wrong stops claiming a narrow "
              "band");
        check(after.sigma_ms > 9000,
              "and the floor is the size of the miss, not a fraction of it");

        // A floor, not a term. The same history must not widen a regime it was
        // never measured in, or one node's bad luck would price every other
        // candidate.
        rf::NodeState other = cluster[0];
        const rf::Estimate elsewhere =
            band->estimate(req, other, ledger, none, rf::now_ms());
        check(elsewhere.sigma_ms < 9000,
              "the floor belongs to the node that earned it, not to the cluster");

        // And it may only widen. A model that is accurate must keep the
        // sharpness Phase 2 bought it, or learning to predict well would cost
        // it the ability to say so.
        auto sharp = rf::make_learned_cost_model(scoring);
        const rf::Estimate sharp_before =
            sharp->estimate(req, node, ledger, none, rf::now_ms());
        for (int i = 0; i < 20; ++i) {
            rf::TraceRecord r = make_record("sim-jetson", "planner:12b", true,
                                            1200, 200, 400, 2000, 1);
            r.cost_model = sharp->name();
            r.predicted_total_ms = r.total_ms;   // dead on, every time
            rf::Candidate c;
            c.node_id = "sim-jetson";
            c.admitted = true;
            r.candidates.push_back(c);
            sharp->observe(r);
        }
        const rf::Estimate sharp_after =
            sharp->estimate(req, node, ledger, none, rf::now_ms());
        check(sharp_after.sigma_ms <= sharp_before.sigma_ms + 1.0,
              "a model that keeps being right is not punished with a wider band");

        // Same trap as D31: a record written by another cost model carries that
        // model's error. Adopting it would floor our band with somebody else's
        // misses.
        auto borrowed = rf::make_learned_cost_model(scoring);
        for (int i = 0; i < 20; ++i) {
            rf::TraceRecord r = make_record("sim-jetson", "planner:12b", true,
                                            1200, 200, 400, 2000, 1);
            r.cost_model = "static-v1";
            r.predicted_total_ms = 2400;
            r.total_ms = 12400;
            rf::Candidate c;
            c.node_id = "sim-jetson";
            c.admitted = true;
            r.candidates.push_back(c);
            borrowed->observe(r);
        }
        const rf::Estimate not_ours =
            borrowed->estimate(req, node, ledger, none, rf::now_ms());
        check(not_ours.sigma_ms < 9000,
              "another model's error does not become our floor (D31, D39)");
    }

    // Footprint: one node holding the model prices a load onto a node that does
    // not (D35). The weights are the same bytes on any GPU, so the overhead
    // over disk size transfers where an absolute byte count would not.
    {
        auto fp = rf::make_learned_cost_model(scoring);
        rf::NodeState holder = cluster[1];          // sim-jetson, holds planner
        rf::NodeState empty = cluster[0];           // sim-desktop, holds nothing
        const uint64_t disk = holder.disk_bytes("planner:12b");
        check(disk > 0, "the fixture node reports a disk size to divide by");

        // Real measurement from this project's card: resident VRAM ran 1.4%
        // above disk size, against a seeded 8% margin plus a KV term.
        holder.models_resident.clear();
        rf::ResidentModel observed;
        observed.name = "planner:12b";
        observed.vram_bytes = static_cast<uint64_t>(disk * 1.014);
        holder.models_resident.push_back(observed);

        const uint64_t seeded = fp->footprint_bytes("planner:12b", empty, 4096);
        for (int i = 0; i < 5; ++i) {
            // Scoring the holder is what harvests the observation.
            fp->estimate(req, holder, ledger, none, rf::now_ms());
        }
        const uint64_t learned = fp->footprint_bytes("planner:12b", empty, 4096);

        check(learned < seeded,
              "an observed footprint is smaller than the seeded margin plus KV");
        check(std::llabs(static_cast<long long>(learned) -
                         static_cast<long long>(disk * 1.014)) < disk / 100,
              "and lands on the ratio that was actually observed");

        // Asking for more context than the engine's default allocates has to
        // cost more, or a large-context request is admitted onto a node that
        // cannot hold it -- an admission failure, not a slow reply.
        const uint64_t roomy = fp->footprint_bytes("planner:12b", empty, 32768);
        check(roomy > learned,
              "context beyond the observed baseline is charged, not ignored");

        // A node that already holds it needs no transfer: that is measurement,
        // not inference, and the seed path already returns it.
        check(fp->footprint_bytes("planner:12b", holder, 4096) == observed.vram_bytes,
              "a node holding the model reports what it actually occupies");
    }

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
    test_admission_trusts_its_own_completion();
    test_residency_limit();
    test_exclusion_binds_even_when_the_node_would_win();
    test_trace_rotation_and_tail();
    test_contention_counted_at_first_token();
    test_warmth_beats_idle_power();
    test_ledger_prevents_duplicate_loads();
    test_queue_and_contention();
    test_uncertainty_and_omission();
    test_round_robin_is_a_baseline();
    test_learned_cost_model();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
