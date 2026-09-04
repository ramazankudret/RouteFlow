# Decision log

Every decision rev 1 of the architecture left open or got wrong, and the call
made in rev 2. One entry per decision. Entries are append-only: a superseded
decision is marked, not deleted.

Status legend: **Settled** (implement as written) · **Provisional** (implement,
but the trace is expected to revise it) · **Deferred**.

---

### D1 — `Estimate` is a declared type · Settled

Rev 1 used `Estimate` as `ICostModel::estimate`'s return type without ever
defining it, while §6's four-term formula, the `Decision` breakdown, the trace
and the UI all depend on its shape.

Defined in ARCHITECTURE §5 with five duration terms, `sigma_ms`, `Confidence`,
`samples` and an `omitted_terms` bitmask. It is the shared contract across all
four consumers, so it lives in `/common/types.h`, not in the cost model.

### D2 — `model_footprint` is learned, not a constant · Settled

Rev 1 made footprint a hard admission filter with no definition of where the
number comes from. Disk size is not VRAM footprint: KV cache scales with context,
partial CPU offload lowers it.

`ICostModel::footprint_bytes(model, node, num_ctx)` is now part of the interface.
Seeded `weights_bytes * 1.15 + kv_bytes(num_ctx)`, corrected to the VRAM delta
observed on the first cold load of that `(node, model)`. The 1.15 margin applies
only while `samples == 0`. A wrong footprint is a *routing* bug, not a latency
bug, so it gets its own error panel in Phase 2.

### D3 — Telemetry is an interface with three backends · Settled

Rev 1 said "NVML, degrade if absent" while claiming the thermal term matters most
on Jetson — where NVML reports no iGPU telemetry at all. Internally inconsistent:
the signal would be empty exactly where it was supposed to pay off.

`ITelemetry` with `NvmlTelemetry` (dlopen, self-declared ABI, `nvml.h` never
included), `TegraTelemetry` (sysfs + `/proc/meminfo`), `NullTelemetry`. Selected
by probe order at agent startup, backend name reported in `/state` and recorded
in every trace record. This is the only place in the tree allowed to carry
platform `#ifdef`s.

### D4 — The router owns a ledger; dispatch is not atomic · Settled

The most serious defect in rev 1. `NodeState` is always stale by up to one sample
interval, so concurrent requests score against the same snapshot and pick the same
node. For a *cold* model they instead pick different nodes and **load the same
model twice**, spending VRAM to manufacture the exact problem RouteFlow exists to
solve.

`NodeLedger` is authoritative and updated synchronously at dispatch, never derived
from telemetry: `inflight`, `reserved_vram_bytes`, `pending(node, model)`,
`concurrent_decoders`. A second request for a model already loading on node A sees
`T_load = remaining_ms` there and queues behind the load rather than duplicating
it. Herd and duplicate-load are one problem with one fix.

### D5 — Decode rate is conditioned on concurrency · Settled

Rev 1 learned a single scalar `decode_rate` per `(node, model)`. Real decode
throughput per request falls as the GPU is shared, so the scalar's error is
*systematic and directionally harmful*: it persistently over-rewards busy warm
nodes, which is the one failure mode this project cannot have.

```
effective_decode_rate = decode_rate(node,model) / (1 + alpha(node) * (concurrent_decoders - 1))
```

`alpha` seeded at 1.0 (perfect fair-share — the correct prior for a saturated
GPU), learned per node. Only `concurrent_decoders == 1` observations update the
base rate; contended observations update `alpha`.

### D6 — `T_queue` is parallel-aware and warm-only · Settled

Rev 1's `inflight × mean_job_duration` assumed serial execution — false for
Ollama with `OLLAMA_NUM_PARALLEL > 1` — and averaged in cold-start jobs whose
duration includes a model load.

`T_queue` is zero while `inflight < engine_slots` and otherwise counts whole
queued rounds; contention among *running* requests is priced in D5 instead.
`mean_warm_job_ms` excludes cold starts. Contention is now priced exactly once.

### D7 — Eviction is admissible and priced · Settled

Rev 1 rejected any node with `vram_free < footprint`, eliminating nodes that could
evict an idle model to make room — the normal case on an 8 GB card. And nothing
charged a request for the cost it imposes on the *next* one by taking the VRAM.

Admission now tests `vram_free_effective + evictable_bytes >= footprint`. Scoring
gains `T_evict = evicted_bytes / load_bandwidth * evict_weight`, applied only when
the victim was used recently (`warm_window`, default 120 s). `evict_weight`
defaults to **0.5 and ON** — without it the policy is self-defeating under memory
pressure. Set it to 0 to reproduce rev 1 behaviour.

### D8 — Uncertainty is first-class · Settled

`T_decode` is usually the largest term and is driven by `predicted_output_tokens`,
the least reliable input. When output prediction is off by 3×, the warmth signal
drowns and the ranking is noise — but rev 1's UI would still present it as a
confident number.

`Estimate` carries `sigma_ms`, dominated by
`predicted_output_sigma / effective_decode_rate`. When `margin_ms` is below the
`sigma_ms` of either top-two candidate, the policy still picks the minimum but
records `decided_by = "within_noise"`, and the UI shows it as such.

### D9 — Failure and retry semantics are enumerated · Settled

Rev 1 said "fail with a clear error"; streaming failure was undefined, and a
silently retried request would corrupt every latency statistic the project
produces.

Retry is permitted **only before the first byte reaches the client**, capped at
one, on the next-best admissible node. After first byte: terminate, no retry.
`outcome` enum: `ok | no_candidate | dispatch_failed | stream_failed |
client_abort | timeout`. Retries appear as separate trace records linked by
`retry_of`.

### D10 — Trace schema v1, versioned from record one · Settled

Rev 1's §10 required versioning from the first commit while its own example
record carried no version field. Fixed, plus: four timestamps instead of one
(clock skew becomes debuggable and every duration is derivable), `outcome` enum
and `error`, `stream`/`num_ctx`/`inflight_at_dispatch`/
`concurrent_decoders_at_dispatch` (D5 and D6 cannot be learned without them),
and **per-candidate state at decision time for losers as well as the winner** —
counterfactual analysis is how the Phase 1 result is defended, and it is
impossible without the losers' numbers.

The learning derivation is written into §7 explicitly, because it is the exact
equation Phase 2 depends on and rev 1 never stated it.

Full reference: `docs/TRACE-SCHEMA.md`.

### D11 — Phase 0 split out of Phase 1 · Settled

Rev 1's Phase 1 was agent + router + two policies + runtime switching + load
generator + report script + UI. Nothing would be demonstrable until all of it
existed, and the trace schema — which everything else reads — would be validated
last.

Phase 0 is plumbing plus `RoundRobin` only; its exit criterion is a real proxied
request and a trace file whose every field is correct. A Phase 1 loss is only
attributable between premise and plumbing if Phase 0 passed first.

### D12 — Phase 1's metric is scenario wall-clock, 5 runs · Settled

Rev 1's exit criterion was "Warmth wins", unquantified. p50, p95 and wall-clock
can move in opposite directions — routing to a warm weak node can improve p50
while damaging p95 — and a single run on three nodes is noise.

Primary metric: **total scenario wall-clock**, median of 5 runs, reported with
IQR. p50/p95 TTFT and total, cold-start count and prediction error reported
alongside but not decisive.

### D13 — Concurrency model stated · Settled

Rev 1 required a concurrent proxy and a mutating cost model without stating a
thread model. One thread per in-flight request from a fixed pool; all shared state
behind one `std::shared_mutex` in `RouterState`; `estimate` shared, `observe`
exclusive; **no I/O and no blocking while holding the lock**. Score under the
shared lock, copy the `Decision` out, release, then dispatch.

### D14 — Auth is a shared bearer token, present from commit one · Settled

Rev 1 said "authenticate node membership" without saying how, and deferring it
means it never gets built.

`Authorization: Bearer <token>` from config, both client→router and router→agent.
Bind `127.0.0.1` by default; `--listen 0.0.0.0` refuses to start without
`--token`. Node addresses come from `nodes.json` only, never from a response body,
so a compromised agent cannot redirect dispatch. The security note also states
plainly what LAN exposure means: every node in `nodes.json` sees every prompt.

### D15 — Token count is estimated and its error is recorded · Settled

Byte-class heuristic, no tokenizer dependency. The trace records
`prompt_tokens_est` **and** engine-reported `prompt_tokens_actual` on every
record, so the error is measured rather than argued about. Revisit only if it
materially moves `T_prefill` ranking.

### D16 — Simulated nodes are a first-class agent mode · Settled

New in rev 2. The Phase 1 exit criterion needs a heterogeneous *and reproducible*
cluster; a comparison run against live hardware with uncontrolled thermals is not
a measurement, and the development machine has one GPU.

`routeflow-agent --simulate <profile.json>` serves the same `/state` and engine
API from a modelled node. The router is never told which nodes are simulated.
Physical validation still happens on real hardware; simulation is what makes the
*comparison* controlled.

### D17 — Predictive placement deferred to Phase 4 · Provisional

Rev 1 committed Phase 3 to learning inter-model transition probabilities and
preloading on them. A single cluster accumulates transition data slowly and
beating LRU is unproven.

Phase 3 ships reactive placement only, benchmarked **against LRU** rather than
against nothing. Predictive placement is reconsidered with Phase 3's trace in
hand — the same discipline §8 applies to regression.

### D18 — Dispatch goes through the agent, not straight to the engine · Settled

Rev 1 was ambiguous: the §4 diagram routes router → agent → engine, while §2's
PAIR note says the router "talks directly to engine endpoints". Implementation
had to pick one.

**The router dispatches to the address in `nodes.json`, and that address is the
agent's.** The agent gains a verbatim streaming proxy for the engine's chat
endpoints (`/v1/chat/completions`, `/api/chat`, `/api/generate`) and forwards to
its own loopback engine.

Three reasons, in order of weight:

1. **Security.** The engine stays bound to `127.0.0.1` on every node. Only the
   agent is exposed, and the agent already authenticates (D14). Dispatching
   straight to the engine would require every node to expose Ollama on the LAN
   unauthenticated — the exact thing §10 warns about.
2. **D16 depends on it.** A simulated node serves `/state` *and* the chat
   endpoints from one process. If real nodes were dispatched to differently, the
   router would be able to tell simulated nodes apart from real ones, and the
   controlled Phase 1 comparison would no longer be controlled.
3. **One address per node.** Node addresses come from the config file only
   (§10), and there is now exactly one of them to configure and to trust.

The cost is one extra hop. It is a byte-for-byte stream forward on the same
machine as the engine, so it adds negligible latency, and the agent measures
nothing and decides nothing — §4.1 still holds: proxying is not scheduling.

`nodes.json` keeps an optional `engine_endpoint` override for the case where an
engine genuinely must be reached directly (an engine on a machine that cannot
run an agent). It is off by default and the trace records which path was used.

### D19 — `load_ms` is unavailable on the OpenAI-compatible path · Open

Found while validating Phase 0 against real hardware, not by reading docs.

Ollama reports `load_duration` on its native endpoints (`/api/chat`,
`/api/generate`) but **not** on `/v1/chat/completions`, whose response carries
only the standard OpenAI `usage` block. Measured, on this machine: a cold start
through `/v1/chat/completions` produced `load_ms: null`; the same model through
`/api/chat` produced `load_ms: 138`.

This matters more than it looks. `T_load` is the term that justifies the entire
project (§6.2), Phase 2 learns `load_bandwidth` from `footprint / load_ms`, and
the OpenAI shape is what agent tooling actually speaks. Left alone, the cost
model would learn load bandwidth only from whatever fraction of traffic happens
to use the native API.

**What is NOT being done:** inventing a value. `load_ms: null` is recorded, which
is the truth (§10: never silently substitute zero). A cold start with a null
load time is visible in the report tool and counted separately.

**Candidates for Phase 1, in preference order:**

1. **Derive it, in its own field.** For a cold start dispatched to an idle node
   (`inflight_at_dispatch == 0`), `ttft_ms - queue_wait_ms` is load plus prefill,
   and prefill on a short prompt is a small fraction of a multi-second load. Add
   `load_ms_source: "engine" | "derived"` so Phase 2 can weight engine-reported
   observations above derived ones. Costs one schema field; a field may be added
   (rule 2).
2. **Ask the node.** The agent already polls `/api/ps` and sees residency
   transitions. It could time one. Rejected for now: a 1 Hz poll cannot resolve
   a load with useful precision, and tightening the poll to measure it would
   make the observer perturb the thing observed.
3. **Translate the request** to the native shape when the target is an Ollama
   node. Rejected: §4.2 dispatches the body untouched, and a translation layer
   is a second place for the request to be wrong.

Option 1 is the plan. Recorded as Open because it is not yet implemented and the
Phase 2 exit criterion depends on it.
