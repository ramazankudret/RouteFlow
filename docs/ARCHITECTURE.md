# RouteFlow — Architecture (rev 2)

**Warmth-aware inference scheduler for heterogeneous local GPU clusters.**

This document is the implementation brief. It defines scope, components, contracts,
data schemas and build order. Read it fully before writing code.

Rev 2 closes the gaps found in the rev 1 review. Every decision that rev 1 left
open is now resolved in the text; `docs/DECISIONS.md` records what changed and why.

---

## 1. Problem

Local agent workloads issue many independent inference requests. Existing local
routers (notably NVIDIA PAIR) dispatch them using queue depth plus a coarse,
smoothed GPU-utilization signal. That policy ignores four things that dominate
real latency on mixed hardware:

1. **GPU class** — a Jetson and an RTX desktop are not interchangeable nodes.
2. **Free VRAM** — a node that cannot hold the model is not a candidate at all.
3. **Model warmth** — a model resident in VRAM answers in milliseconds; a cold
   model costs seconds of load time first.
4. **Request cost** — a 50-token reply and a 4000-token reply are not the same job.

The consequence: an idle powerful node can finish *later* than a busy weak node
that already holds the model warm. RouteFlow exists to make that decision correctly,
and to explain the decision it made.

## 2. Scope

**In scope**

- Measuring every dispatched job and persisting a trace.
- Learning a per-node, per-model cost model from those traces.
- Scoring candidate nodes by predicted completion time.
- Managing which models stay resident on which node.
- A UI that shows where each job went **and why**.

**Out of scope (do not build these)**

- Pooling VRAM, sharding a model across machines, or splitting one in-flight
  request between nodes. One request runs on exactly one node.
- Training or fine-tuning models.
- Replacing the inference engines. RouteFlow drives Ollama and LM Studio; it does
  not implement inference.
- Cloud fallback.

**Relationship to PAIR** — RouteFlow is independent. It talks directly to engine
endpoints and does not require PAIR to be installed. The scoring policy is kept
behind a narrow interface so it can later be proposed upstream as a PAIR policy.

## 3. Technology

Mirror the GPUFlow stack. It is proven in this codebase family and keeps the
dependency surface near zero.

| Layer | Choice |
| --- | --- |
| Core | C++17 |
| GPU telemetry | Pluggable backend (§4.1.1). NVML is `dlopen`ed, never linked. |
| Transport | Dependency-free HTTP/1.1 server + client, SSE for live updates |
| UI | Vanilla JS + canvas, served from the router process |
| Persistence | JSONL append-only files. No database. |
| Build | CMake ≥ 3.16 |

**No third-party libraries.** JSON, HTTP and SSE are implemented in `/common`.
This is a hard constraint, not a preference: the project must build on a fresh
Jetson with nothing but a compiler.

Platform target: Linux first. Jetson (aarch64) must build from the same tree with
no `#ifdef` outside `/agent/telemetry`. Windows is a later concern.

## 4. Components

Three deployable pieces. Keep them in one repository, three targets.

```
                    ┌──────────────────────────────┐
   agent / client ─▶│  routeflow-router            │
   (OpenAI API)     │   admission → scoring →      │
                    │   dispatch → trace           │
                    └───────┬──────────────┬───────┘
                            │              │  SSE
                     HTTP   │              ▼
                            │        routeflow-ui
              ┌─────────────┼─────────────┐
              ▼             ▼             ▼
        node-agent    node-agent    node-agent
         (Jetson)      (desktop)     (laptop)
              │             │             │
           Ollama /     Ollama /      LM Studio
           LM Studio    LM Studio
```

### 4.1 `routeflow-agent` (per node)

One small service per machine. Read-only with respect to the GPU; it observes,
it does not allocate.

Responsibilities:

- Sample GPU telemetry through the backend chosen at startup (§4.1.1): device
  name, total/free VRAM, utilization, temperature, power draw and, where exposed,
  the power cap.
- Poll the local engine for the list of models available on disk and the list
  currently resident in memory. These are **different sets** and must be reported
  separately — the distinction is the core of this project.
- Report engine liveness, engine parallel-slot capacity, and in-flight count.
- Serve the aggregate as `GET /state` and stream deltas over SSE at `/events`.

It contains no scheduling logic. It never decides anything.

#### 4.1.1 Telemetry backends

Rev 1 said "NVML, degrade if absent" while also placing the thermal/power term's
main value on Jetson — where NVML does not report iGPU telemetry at all. That is
a contradiction: the signal would be empty exactly where it matters most. So
telemetry is an interface with three implementations, selected in this order at
agent startup:

```cpp
class ITelemetry {
public:
    virtual ~ITelemetry() = default;
    virtual const char* name() const = 0;          // "nvml" | "tegra" | "null"
    virtual bool        probe()      = 0;          // true if usable on this host
    virtual bool        sample(std::vector<GpuSample>& out) = 0;
};
```

1. **`NvmlTelemetry`** — `dlopen("libnvidia-ml.so.1")`, resolve the handful of
   symbols used, self-declared ABI. `nvml.h` is **not** included; it is absent on
   Jetson and on hosts without the CUDA toolkit.
2. **`TegraTelemetry`** — sysfs. Utilization `/sys/devices/gpu.0/load` (per-mille),
   thermal zones under `/sys/devices/virtual/thermal/`, power rails under
   `/sys/bus/i2c/drivers/ina3221*/`. VRAM is unified memory: total/free come from
   `/proc/meminfo`, and `vram_total_bytes` is reported as usable system RAM.
3. **`NullTelemetry`** — always succeeds, reports zeros with `telemetry_ok=false`.
   The router must route correctly on a node with no GPU telemetry; it simply
   loses the thermal and utilization terms there.

The backend name is reported in `/state` and recorded in the trace. A term
computed from an unavailable signal is never silently treated as zero — it is
omitted from the score and marked as omitted in the `Decision` breakdown.

#### 4.1.2 Simulated nodes

`routeflow-agent --simulate <profile.json>` presents a fully synthetic node —
declared VRAM, load bandwidth, prefill/decode rates, jitter, and a fake engine
that sleeps for the modelled duration — behind the same `/state` and engine API.

This is not a toy. The Phase 1 exit criterion requires a *heterogeneous* cluster,
and a reproducible one; a policy comparison run against live hardware with
uncontrolled thermals is not a measurement. Simulated nodes make the comparison
deterministic and let the whole pipeline be exercised before any second machine
exists. Real and simulated nodes are indistinguishable to the router, which is
the point: the router is never told which is which.

### 4.2 `routeflow-router` (one per cluster)

The scheduler. Pipeline, in order:

1. **Ingest** — accept an OpenAI-compatible `/v1/chat/completions` request.
   Also accept the Ollama-native shape so existing tools work unchanged.
2. **Estimate** — ask the cost model how long this request would take on each node.
3. **Admit** — filter out nodes that cannot serve it at all (§6).
4. **Score** — rank the admitted nodes by predicted completion time.
5. **Reserve** — commit the choice to the ledger (§6.3) *before* dispatching.
6. **Dispatch** — proxy the request to the winner, streaming the response back
   untouched.
7. **Record** — release the reservation, append a trace record, update the cost model.

The router holds all state in memory and rebuilds it from the trace log on start.

#### 4.2.1 Concurrency model

Rev 1 left this unstated while requiring a concurrent proxy and a mutating cost
model. Decided:

- One thread per in-flight request from a fixed-size pool (dispatch is I/O-bound).
- **All shared state** — the ledger, the cost model, the node registry — lives
  behind a single `std::shared_mutex` owned by `RouterState`.
- `ICostModel::estimate` is `const` and takes a shared lock. `observe` takes an
  exclusive lock.
- **No component may perform I/O or block while holding the lock.** Score under
  the shared lock, copy the `Decision` out, release, then dispatch. This is a
  reviewable invariant, not a guideline.

### 4.3 `routeflow-ui`

Served by the router. Shows the cluster, live per-node GPU and VRAM, resident
models per node, and the job feed.

The distinguishing feature is the **decision panel**: selecting a job shows the
score breakdown for every candidate node — the predicted queue wait, load,
prefill and decode components, the uncertainty on each total, and which term
decided the outcome. A user should be able to disagree with the scheduler and see
exactly why it chose what it chose.

## 5. Interfaces

Keep these narrow. Everything behind them is replaceable, and the phases in §9
each swap one implementation without touching the others.

```cpp
// ---- State ------------------------------------------------------------------

struct ResidentModel {
    std::string name;
    uint64_t    vram_bytes;
    int64_t     last_used_ms;      // epoch ms; 0 if never observed by us
};

// A candidate machine and its live state. Produced by the agent layer.
struct NodeState {
    std::string        id;
    std::string        gpu_name;
    std::string        telemetry_backend;   // "nvml" | "tegra" | "null"
    bool               telemetry_ok;        // false => util/power/temp are absent
    uint64_t           vram_total_bytes;
    uint64_t           vram_free_bytes;
    float              gpu_util;            // 0.0 - 1.0, smoothed
    float              power_watts;
    float              power_cap_watts;     // 0 if unknown
    float              temperature_c;
    EngineKind         engine;              // Ollama | LMStudio | Simulated | None
    bool               engine_healthy;
    uint32_t           engine_slots;        // parallel requests the engine accepts
    std::vector<std::string>   models_on_disk;
    std::vector<ResidentModel> models_resident;
    uint32_t           inflight_reported;   // engine's own view; advisory only
    int64_t            sampled_at_ms;
};

// What the router knows about an incoming request before dispatch.
struct RequestFeatures {
    std::string  model;
    uint32_t     prompt_tokens;           // estimated; see §12
    uint32_t     predicted_output_tokens; // from the cost model
    float        predicted_output_sigma;  // 1-sigma, tokens
    uint32_t     num_ctx;                 // requested context window, 0 = engine default
    bool         stream;
    std::string  role_hint;               // optional; see §8
};

// ---- Estimation -------------------------------------------------------------

enum class Confidence { Seeded, Learning, Converged };

// Rev 1 used this type without ever declaring it. It is the shared contract
// between cost model, policy, Decision breakdown, trace and UI.
struct Estimate {
    double t_queue_ms   = 0;
    double t_load_ms    = 0;
    double t_prefill_ms = 0;
    double t_decode_ms  = 0;
    double t_evict_ms   = 0;   // externality of evicting a warm model (§6.2)

    double sigma_ms     = 0;   // 1-sigma on the total
    Confidence conf     = Confidence::Seeded;
    uint32_t   samples  = 0;   // observations backing the dominant rate

    uint32_t   omitted_terms = 0;  // bitmask; a term dropped for lack of signal

    double total_ms() const {
        return t_queue_ms + t_load_ms + t_prefill_ms + t_decode_ms + t_evict_ms;
    }
};

// Predicts durations. Phase 2 replaces the static implementation.
class ICostModel {
public:
    virtual ~ICostModel() = default;

    // Thread-safe under a shared lock; must not block.
    virtual Estimate estimate(const RequestFeatures&, const NodeState&,
                              const NodeLedger&) const = 0;

    // VRAM a model needs on this node at this context size. Admission depends
    // on it, so it is part of the interface rather than a hidden constant.
    virtual uint64_t footprint_bytes(const std::string& model,
                                     const NodeState&, uint32_t num_ctx) const = 0;

    // Expected output length. Split out because its error must be tracked
    // separately from timing error (§8).
    virtual OutputPrediction predict_output(const RequestFeatures&) const = 0;

    // Exclusive lock. Called once per completed job.
    virtual void observe(const TraceRecord&) = 0;

    virtual const char* name() const = 0;
};

// ---- Decision ---------------------------------------------------------------

enum class AdmitReason { Ok, EngineDown, ModelMissing, InsufficientVram,
                         Excluded, NodeStale };

struct Candidate {
    std::string  node_id;
    bool         admitted;
    AdmitReason  reason;
    Estimate     est;
    // Node state at decision time — required for counterfactual analysis of
    // the losers, not just the winner.
    uint64_t     vram_free_bytes;
    float        gpu_util;
    bool         was_resident;
    uint32_t     inflight;
};

struct Decision {
    std::string             winner_node_id;   // empty => no candidate
    std::vector<Candidate>  candidates;       // every node, admitted or not
    std::string             decided_by;       // term with largest gap to runner-up
    double                  margin_ms;        // winner vs runner-up
    std::string             policy_name;
};

// Chooses a node. Phase 1 ships two: RoundRobin (baseline) and Warmth.
class IPolicy {
public:
    virtual ~IPolicy() = default;
    virtual Decision select(const RequestFeatures&,
                            const std::vector<NodeState>&,
                            const NodeLedger&,
                            const ICostModel&) const = 0;
    virtual const char* name() const = 0;
};

// Talks to one engine family. Isolates Ollama/LM Studio differences.
class IEngineAdapter {
public:
    virtual ~IEngineAdapter() = default;
    virtual std::vector<ResidentModel> resident(const NodeState&) const = 0;
    virtual bool preload(const NodeState&, const std::string& model) = 0;
    virtual bool evict(const NodeState&, const std::string& model) = 0;
};
```

`Decision` carries the full per-node score breakdown, not just the winner. The
UI and the evaluation harness both depend on it, and discarding it early is the
single easiest way to make this project unfalsifiable.

The active policy must be switchable **at runtime** via config or the admin
endpoint `POST /admin/policy`. Comparing policies requires running the same
workload against each without a rebuild.

## 6. Admission and scoring

### 6.1 Admission — hard filters, applied first

A node is ineligible if any of these hold:

- The engine is not running or not healthy.
- Its last `/state` sample is older than `node_stale_ms` (default 5000).
- The model is not on disk (Phase 3 may relax this once fetch cost is modelled).
- It cannot make room: `vram_free_effective + evictable_bytes < footprint`
  and the model is not already resident.
- The node is in a user-configured excluded state.

where

```
vram_free_effective = vram_free_bytes - ledger.reserved_vram_bytes(node)
evictable_bytes     = Σ bytes of resident models not currently serving a request
```

Rev 1 used `vram_free < footprint` alone, which wrongly eliminates any node that
could evict an idle model to make room — the common case on an 8 GB card holding
two models. Eviction is now admissible and *priced* (§6.2) rather than forbidden.

`footprint` comes from `ICostModel::footprint_bytes`, seeded as

```
footprint = weights_bytes * 1.15 + kv_bytes(num_ctx)
```

`weights_bytes` starts from the engine-reported on-disk size and is corrected to
the VRAM delta actually observed on the first cold load of that model on that
node. `kv_bytes` starts from a per-architecture table and is corrected the same
way. The 1.15 margin applies only while `samples == 0`.

If no node is admissible, fail with `503` and an error naming the binding
constraint per node. Do not silently queue forever.

### 6.2 Scoring — predicted completion time, lowest wins

```
T_total = T_queue + T_load + T_prefill + T_decode + T_evict

slots   = node.engine_slots
T_queue = 0                                            if inflight < slots
        = ceil((inflight - slots + 1) / slots) * mean_warm_job_ms(node)   otherwise

T_load  = 0                                            if model resident
        = remaining_ms(pending_load)                   if already loading (§6.3)
        = model_bytes / load_bandwidth(node)           otherwise

T_prefill = prompt_tokens / prefill_rate(node, model)

T_decode  = predicted_output_tokens / effective_decode_rate
effective_decode_rate = decode_rate(node, model)
                        / (1 + alpha(node) * (concurrent_decoders - 1))

T_evict = 0                              if no eviction needed, or evicted model is cold
        = evicted_bytes / load_bandwidth(node) * evict_weight    otherwise
```

Every rate on the right-hand side is a learned per-node, per-model quantity.
Phase 1 seeds them with static defaults; Phase 2 learns them.

Three points, each of which rev 1 got wrong:

**Contention is priced exactly once.** `T_queue` covers *true* queueing — more
in-flight requests than the engine has parallel slots. Slowdown from sharing the
GPU among requests that are all running is priced in `effective_decode_rate`, not
in `T_queue`. Rev 1's `inflight × mean_job_duration` assumed serial execution and
therefore double-counted on any engine with `OLLAMA_NUM_PARALLEL > 1`.
`alpha(node)` is the contention coefficient, learned per node, seeded at 1.0 —
perfect fair-share, the physically correct prior for a saturated GPU. Learning a
single unconditioned `decode_rate` would bias the model toward busy warm nodes,
which is precisely the failure mode this project must not have.

**`mean_warm_job_ms` excludes cold starts.** Averaging jobs that paid a load cost
into the queue term inflates it and makes cold nodes look busy.

**`T_load` is the term that justifies the project.** Model it explicitly and never
fold it into a generic "warmness bonus" — the whole point is that it is a
*measured number of seconds*, comparable against the others. `T_evict` is its
mirror: the seconds the *next* request will pay because this one took the VRAM.
Without it the policy is self-defeating under memory pressure, so `evict_weight`
defaults to 0.5 and ON. Set it to 0 to reproduce rev 1 behaviour.

**Uncertainty.** `sigma_ms` is dominated by the output-length prediction:
`sigma_ms ≈ predicted_output_sigma / effective_decode_rate`, combined in
quadrature with the load-time residual. When `margin_ms` is smaller than the
`sigma_ms` of either of the top two candidates, the decision is not
distinguishable from noise; the policy still picks the lowest total, but records
`decided_by = "within_noise"`. An honest scheduler says when it is guessing.

Optional penalty terms, off by default, behind config flags:

- **Thermal/power** — penalise a node near its power cap or thermal limit. This
  matters on Jetson and is where RouteFlow diverges most from any existing
  router. Requires `telemetry_ok`; otherwise the term is omitted and flagged.

### 6.3 The ledger — dispatch is not atomic

`NodeState` arrives by poll and SSE, so it is always stale by up to one sample
interval. Two concurrent requests scored against the same snapshot will both see
an idle warm node and both pick it. Worse, for a *cold* model they will pick two
different nodes and load the same model twice — burning VRAM in order to create
exactly the problem RouteFlow exists to solve.

The router therefore keeps its own authoritative ledger, updated synchronously at
dispatch, never derived from telemetry:

```cpp
struct NodeLedger {
    uint32_t inflight(NodeId) const;              // ++ at dispatch, -- at completion
    uint64_t reserved_vram_bytes(NodeId) const;   // Σ footprints of pending loads
    // A load this router started that has not completed yet.
    struct PendingLoad { std::string model; int64_t started_ms; double eta_ms; };
    std::optional<PendingLoad> pending(NodeId, const std::string& model) const;
    uint32_t concurrent_decoders(NodeId) const;
};
```

Scoring consequence: if a model is already being loaded on node A, a second
request for that model sees `T_load = remaining_ms` on A — usually far cheaper
than a full cold load elsewhere — so it queues behind the load instead of
duplicating it. The herd problem and the duplicate-load problem are the same
problem, and the ledger is the single fix for both.

The reservation is taken in step 5 of §4.2, under the exclusive lock, after
scoring and before the dispatch I/O begins.

### 6.4 Failure semantics

Rev 1 said "fail with a clear error" and stopped there. Decided:

| When | Behaviour | `outcome` |
| --- | --- | --- |
| No admissible node | `503`, per-node reason in body | `no_candidate` |
| Connect/HTTP error before any byte reached the client | transparently retry once on the next-best admissible node | failed attempt: `dispatch_failed`, then a new job record |
| Error after the first byte reached the client | terminate the stream; do not retry | `stream_failed` |
| Client disconnects | abort upstream, release ledger | `client_abort` |
| Exceeds `request_timeout_ms` | abort upstream | `timeout` |
| Normal completion | — | `ok` |

Retries are capped at one and are recorded as separate trace records linked by
`retry_of`. A retry that is invisible in the trace would corrupt every latency
statistic the project produces.

## 7. Trace schema

Append-only JSONL, one record per completed job. This file is the project's
primary asset — the cost model, the UI history and the evaluation harness all
read it. **The format is a contract; it is versioned from the first commit.**
Full field reference and version history live in `docs/TRACE-SCHEMA.md`.

```json
{
  "v": 1,
  "job_id": "01J9ZQ8F3K2M7X",
  "retry_of": null,
  "ts_received":    "2026-09-03T14:21:07.412Z",
  "ts_dispatched":  "2026-09-03T14:21:07.455Z",
  "ts_first_token": "2026-09-03T14:21:11.765Z",
  "ts_done":        "2026-09-03T14:21:15.332Z",
  "model": "qwen4:12b",
  "role_hint": "planner",
  "num_ctx": 8192,
  "stream": true,
  "policy": "warmth-v1",
  "cost_model": "static-v1",
  "node_id": "jetson-01",
  "was_resident": false,
  "prompt_tokens_est": 1795,
  "prompt_tokens_actual": 1842,
  "output_tokens": 312,
  "predicted_output_tokens": 280,
  "predicted_output_sigma": 96,
  "predicted_total_ms": 8100,
  "predicted_sigma_ms": 1400,
  "queue_wait_ms": 40,
  "load_ms": 3980,
  "ttft_ms": 4310,
  "total_ms": 7920,
  "inflight_at_dispatch": 1,
  "concurrent_decoders_at_dispatch": 1,
  "gpu_util_at_dispatch": 0.12,
  "vram_free_at_dispatch": 21903073280,
  "telemetry_backend": "nvml",
  "evicted": [],
  "decided_by": "t_load",
  "margin_ms": 1500,
  "candidates": [
    { "node_id": "jetson-01", "admitted": true, "predicted_total_ms": 8100,
      "t_queue": 40, "t_load": 3980, "t_prefill": 180, "t_decode": 3900,
      "t_evict": 0, "sigma_ms": 1400, "conf": "seeded",
      "vram_free": 21903073280, "gpu_util": 0.12, "was_resident": false,
      "inflight": 1 },
    { "node_id": "desktop-01", "admitted": true, "predicted_total_ms": 9600,
      "t_queue": 2100, "t_load": 0, "t_prefill": 900, "t_decode": 6600,
      "t_evict": 0, "sigma_ms": 2200, "conf": "seeded",
      "vram_free": 3221225472, "gpu_util": 0.81, "was_resident": true,
      "inflight": 3 },
    { "node_id": "laptop-01", "admitted": false, "reason": "insufficient_vram",
      "vram_free": 1073741824, "gpu_util": 0.02, "was_resident": false,
      "inflight": 0 }
  ],
  "outcome": "ok",
  "error": null
}
```

Two properties this schema must preserve:

**Prediction error is visible from day one.** Both `predicted_total_ms` and
`total_ms` are on every record, as are predicted and actual output tokens. These
are two different errors with two different causes (§8) and are never combined
into one number.

**The learning derivation is closed.** Phase 2 recovers the rates it needs from
fields present here, with no additional instrumentation:

```
t_prefill_actual = ttft_ms - load_ms - queue_wait_ms
t_decode_actual  = total_ms - ttft_ms
prefill_rate     = prompt_tokens_actual / t_prefill_actual
decode_rate      = output_tokens / t_decode_actual   (only when
                   concurrent_decoders_at_dispatch == 1; otherwise it feeds alpha)
load_bandwidth   = footprint_observed / load_ms
```

Losing candidates carry their own state at decision time. Counterfactual analysis
— "would the other node actually have been faster?" — is impossible without it,
and that analysis is how the Phase 1 exit criterion is defended.

A corrupt line is skipped with a counter, never fatal (§10).

## 8. Cost model (Phase 2)

Split the prediction rather than fitting one function. The parts have different
shapes and are each easy to learn.

**Load bandwidth.** Roughly linear in model size for a given node. One EWMA per
node. A handful of cold starts is enough to make this useful.

**Footprint.** Per `(node, model, ctx_bucket)`: the VRAM delta observed across a
cold load. Seeded from on-disk size × 1.15 + a per-architecture KV table.
Admission depends on this, so a wrong value is a routing bug, not a latency bug —
it gets its own accuracy panel.

**Generation rates.** `prefill_rate` and `decode_rate` per `(node, model)`, EWMA,
seeded from a static table keyed by GPU class so a fresh cluster is not useless on
its first request. **Only observations at `concurrent_decoders == 1` update the
base rate.** Observations under contention update `alpha(node)` instead, by
solving §6.2's contention equation for alpha. Conflating the two is the single
most consequential modelling error available here.

**Output length.** Not knowable before the request. EWMA of observed output
tokens per `(model, role_hint)`, defaulting to a per-model average when no hint is
present, and an EWMA of absolute error alongside it — that error *is*
`predicted_output_sigma`, which is what makes §6.2's noise check work. Track this
error separately from timing error; they fail for different reasons and
conflating them will mislead you.

EWMA half-lives are configuration, not constants: `load_bw`, `rates` and
`output_len` each get their own, defaulting to 20 observations. A cluster whose
hardware changes should forget at a rate the operator chooses.

Do not reach for regression until the EWMA baseline is measured and its error
quantified. It may already be sufficient. If it is not, the trace file already
holds everything a regression would need, and swapping `ICostModel` is a
contained change.

`role_hint` is an optional field a caller may pass (`X-RouteFlow-Role` header or
a `routeflow.role` request extension) to say what kind of step this is. Never
require it.

## 9. Build order

Each phase produces something demonstrable. Do not begin a phase before the
previous one is measured.

### Phase 0 — Plumbing

Rev 1's Phase 1 contained agent, router, two policies, runtime switching, a load
generator, a report script and a UI. Nothing would have been demonstrable until
all of it existed, and the trace schema — the asset everything else reads — would
have been validated last. Split:

- `/common`: JSON, HTTP/1.1 server + client, SSE, config, logging, ULID.
- `routeflow-agent`: telemetry backends, Ollama probe, `/state`, `/events`.
- `routeflow-router`: ingest, ledger, dispatch, trace writer, `RoundRobin` only.
- `--simulate` node profiles.

**Exit criterion:** a real request proxied through the router to Ollama, and a
trace file whose every field is populated with a correct value. Verify the §7
derivation on real records before writing a policy that depends on it.

### Phase 1 — Measure

- `StaticCostModel` with the seed tables, footprint estimation, ledger-aware
  `T_load`.
- `Warmth` policy. Runtime policy switching via `POST /admin/policy`.
- Load generator: replays a fixed multi-agent scenario (one planner model plus
  several concurrent sub-agent calls on a smaller model) reproducibly, from a
  seeded RNG.
- Report script: reads N trace files, emits **scenario wall-clock** (primary),
  plus p50/p95 TTFT and total, cold-start count, and prediction error, per policy.
- Minimal UI: nodes, live telemetry, job feed, decision panel.

**Exit criterion:** `Warmth` beats `RoundRobin` on **total scenario wall-clock**,
median of **5 runs**, reported with the interquartile range, on a heterogeneous
cluster of at least three nodes (simulated nodes count). p50 and p95 latency are
reported alongside but are not the decision metric — routing to a warm weak node
can improve p50 while damaging p95, and for agent workloads the honest question is
when the whole batch finished.

If `Warmth` does not win, the premise is wrong and that is worth knowing
immediately. That verdict is only trustworthy if Phase 0's trace validation
passed — otherwise a loss cannot be attributed between premise and plumbing.

### Phase 2 — Learn

- `LearnedCostModel` implementing `ICostModel`, fed by `observe()`.
- Cold-start seeding from the static table; `alpha` learned from contended jobs.
- Prediction-error panel in the UI: timing error, output-length error and
  footprint error shown separately.

**Exit criterion:** measurably lower prediction error than the static model on
replayed traces, *and* a wall-clock improvement over Phase 1's `Warmth` on the
same scenario. Both, not either.

### Phase 3 — Manage warmth

- `PlacementManager`: decides which models stay resident where, using
  `IEngineAdapter::preload` and `evict`.
- Reactive first — keep frequently-requested models resident on the node that
  serves them, evict the stale. Compare against plain LRU, which is the real
  baseline here.

**Exit criterion:** cold-start count per scenario run drops substantially against
LRU, with no regression in p95.

Predictive placement — learning transition frequencies between models from the
trace and preloading ahead of the request — is **deferred to Phase 4 and is not
committed to**. A single-cluster trace accumulates transition data slowly, and
beating LRU with it is unproven. Measure reactive placement first and decide with
data, exactly as §8 refuses to reach for regression before the EWMA is measured.

## 10. Design constraints

- **One responsibility per component.** The agent observes. The cost model
  predicts. The policy chooses. The ledger accounts. The placement manager acts.
  A change to the scoring formula must not touch telemetry code.
- **Depend on the interfaces in §5, not on concrete types.** All phases are
  implementations swapped behind them.
- **Every decision must be explainable.** If a code path can influence routing
  without appearing in the `Decision` breakdown, it is a bug. This includes the
  ledger: reserved VRAM and pending loads appear in the breakdown.
- **Never silently substitute zero for a missing signal.** An unavailable term is
  omitted and marked omitted, in both `Estimate` and the UI.
- **Degrade, do not crash.** Missing NVML, a dead engine, an unreachable node, a
  corrupt trace line and a node that vanishes mid-request must all be survivable.
- **The trace format is a contract.** `v` is present from the first record. The
  evaluation harness must read old runs; a field may be added, never repurposed.
- **Security, from the first commit.** The router binds `127.0.0.1` by default.
  `--listen 0.0.0.0` refuses to start without `--token`. Router→agent and
  client→router both authenticate with `Authorization: Bearer <token>` from
  config; agents reject unauthenticated `/state`. Node addresses come from the
  config file only — never from a response body — so a compromised agent cannot
  redirect dispatch. Requests are proxied verbatim to LAN machines: enabling LAN
  exposure means trusting every node in `nodes.json` with every prompt.

## 11. Repository layout

```
/common         JSON, HTTP/1.1, SSE, config, log, ULID  (no third-party deps)
/agent          node-side service
  /telemetry    ITelemetry: nvml (dlopen), tegra (sysfs), null
  /engine       local engine probes
  /sim          simulated node
/router         scheduler, ingest, dispatch, trace
  /core         RouterState, NodeLedger, TraceWriter, registry
  /policy       IPolicy implementations
  /cost         ICostModel implementations
  /engine       IEngineAdapter implementations (ollama, lmstudio)
/ui             static assets served by the router
/bench          load generator + report scripts + node profiles
/docs           this file, DECISIONS.md, TRACE-SCHEMA.md
```

## 12. Open questions

Resolve these during Phase 1; they are not blockers to starting.

- **Token counting.** `prompt_tokens` is estimated without a tokenizer
  dependency: a byte-class heuristic (~4 B/token Latin, ~1.5 CJK, with a
  correction for accented scripts). The trace records the estimate *and* the
  engine-reported actual on every record, so the error is measured from day one
  rather than argued about. Revisit only if the error materially moves
  `T_prefill` ranking.
- **Node discovery.** Static `nodes.json` only. Automatic discovery is a
  convenience with a real security cost (§10).
- **`T_queue` granularity.** Whether it needs per-model resolution or a per-node
  mean suffices. Answer with the trace data rather than by argument.
- **`alpha` shape.** The linear fair-share model in §6.2 is a prior, not a law.
  Whether contention is better modelled per-node, per-(node,model), or
  non-linearly is a Phase 2 question the trace can answer.
