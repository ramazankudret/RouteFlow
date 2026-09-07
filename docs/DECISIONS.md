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

**Update, Phase 1.** The simulated node reproduces the gap exactly — it emits
`load_duration` on the Ollama-native shape and not on the OpenAI one, because it
mirrors what the real engine does. That is the right fidelity to have, and it
means the benchmark inherits the problem rather than papering over it: a
streamed OpenAI bench run records `load_ms: null` on every cold start, so even
in simulation there is currently nothing for Phase 2 to learn load bandwidth
from. This moves option 1 from "needed eventually" to "needed before Phase 2
starts".

### D20 — The hardware seed table is configuration, not a constant · Settled

Found by the first Phase 1 comparison run, which produced a result that looked
like a verdict and was actually a bug: Warmth finished 32% *slower* than
RoundRobin and sent 8 of 10 requests to the weakest node in the cluster.

The cause was in `StaticCostModel`. Its seed table matched a substring of
`gpu_name`, and the entries were keyed by GPU *generation* — `"rtx 40"`,
`"rtx 20"`. Two of the three nodes matched nothing and both fell to the same
generic default, so the cost model believed a fast desktop and a weak laptop
had identical decode rates. With node speed invisible, the only signal left in
the score was `T_load`, and Warmth dutifully sent everything to whichever node
was already warm — including a node three times too slow for the work.

This is worth stating plainly because of how it failed. Nothing crashed, no
term was missing, every trace record was well-formed, and the decision
breakdown was internally consistent. The scheduler explained its choices
confidently and the explanations were wrong at the root, because one input was
a fabricated constant.

Two changes:

1. **Generation-wide entries are gone.** Memory bandwidth varies more within a
   GPU generation than between generations — a 4060 Laptop is 272 GB/s, a 4090
   is 1008 — so a generation-wide entry silently hands a laptop part a desktop
   flagship's throughput. The built-ins are now specific parts, with generation
   fallbacks pinned to the conservative end of each.
2. **`cost.gpu_seeds` in the router config is consulted first.** An operator
   who knows their hardware states it, longest match wins, and no rebuild is
   needed to teach the router about a machine it has not met. Unmatched
   hardware still gets a generic seed, but now says so once per device: a wrong
   seed is a routing error, and a routing error nobody is told about is the
   worst kind.

The Phase 1 bench ships `bench/router.json` with seeds for its simulated
cluster, set deliberately within about 10% of the rates those nodes actually
deliver, in both directions. Exact seeds would hand the cost model the answer
and make the comparison meaningless; Phase 2 exists to remove a residual error,
so there has to be one.

This is also the first concrete argument for Phase 2 that did not come from the
architecture document: a static table is not merely less accurate than a
learned model, it is *silently* wrong on hardware nobody wrote an entry for,
which on a heterogeneous cluster is the normal case.

### D21 — The ledger prices duplicate loads; it does not forbid them · Settled

Rev 2's §6.3 claimed that a second request for a model already loading on node A
"queues behind the load instead of duplicating it", and called the herd problem
and the duplicate-load problem "the same problem". Writing the test for it showed
the claim was both stronger than the code and stronger than the truth.

At the instant of reservation, `remaining_ms` on the loading node equals a fresh
load elsewhere, so the two are priced identically — and that is correct. With
VRAM free on both nodes, loading in parallel genuinely finishes sooner for the
client than queueing one request behind the other's load *and* its generation.
Refusing the duplicate would be slower, in exchange for VRAM nobody was
competing for.

The pathology worth preventing is narrower: loading the same model twice **while
VRAM is scarce**, evicting other models to do it. `reserved_vram_bytes` covers
that by making the loading node correctly look fuller to a concurrent scorer,
and `T_evict` charges whoever displaces a warm model. The herd is broken by
`T_queue` charging the busy node, not by a rule against duplication.

The pending-load path still earns its place: a load that is 80% done is priced at
20% of a fresh one, so a request arriving late in a load does queue behind it.

§6.3 has been corrected. D4 stands — the ledger is still the fix for scoring
against stale telemetry — but its scope is what the ledger actually does.

### D22 — `/snapshot`: a read-only collector envelope · Settled

NoteFlow aggregates several collectors onto one canvas. RouteFlow joins as the
third, but in a different status from the others: AgentFlow and GPUFlow observe
and touch nothing, while RouteFlow *manages* — it changes model residency,
routes requests and evicts.

So the integration is deliberately one-way. `GET /snapshot` publishes the read
state RouteFlow already exposes, wrapped in the shared envelope. The control
surface — `/v1/chat/completions`, `/api/chat`, `/api/generate`, `/admin/*` — is
not reachable through it and gains no shortcut. Management stays in RouteFlow's
own interface. This is not a concession: the control surface stays where it is
while the observation data becomes visible on another plane.

Three things this does *not* do, each of which would have been easy:

- **No new measurement.** `nodes` is `registry.to_json()` verbatim;
  `jobs_recent` counts records already retained for the UI's job feed. No new
  counter, no new collection path, `ITelemetry` untouched.
- **No new authentication path.** `/snapshot` sits behind the same bearer token
  as every other read endpoint (D14). No loopback exemption, no separate key.
- **No coupling of versions.** The envelope's `v` is the envelope's own and is
  deliberately not tied to `TRACE-SCHEMA.md`. A trace bump to v2 leaves the
  envelope at 1; the wrapper and the body have separate lifetimes.

D8's discipline crosses the wire with the data. `p50_ms` over zero completed
jobs is `null`, not `0` — a consumer renders `null` as "—" and would otherwise
draw a latency nobody measured. Likewise a node whose engine cannot report
residency is not listed as holding nothing; it is named in
`residency_unknown_nodes` so the distinction survives the hop.

**A defect this surfaced.** The envelope carries `now_ns`, an epoch timestamp in
nanoseconds — about 1.8e18, well above the 2^53 ceiling for exact integers in a
double. `Json` stored every number as a double, so the field both lost precision
and serialised as `1.78854201051094e+18`. `Json` now keeps an exact `int64`
alongside the double for integer values, and the parser recovers one for any
integer literal without a fraction or exponent. This was latent before
`/snapshot` existed and would have bitten any future field above 2^53; the
selftest now pins it.

**Two later additions to the envelope**, both in `collector`/top level and
neither touching the body:

- `collector.pid` — the router's own process id. It is what a passive observer
  matches a socket's owner against when it checks the family's read
  declarations against actual traffic, and it matches on pid rather than port
  because a port is reused and a pid is not. Its limit is worth stating: a pid
  only means something to an observer on the same host. A NoteFlow reading this
  across a network gets a number it cannot resolve, and should ignore it rather
  than treat it as an identity.
- `reads` — which other collectors in the family this process reads. It is
  `[]` here, and that is a claim rather than a gap: the router polls its own
  agents, which are RouteFlow's components and not separate collectors, and it
  reads no other flow. **Absent and empty are different.** A missing key means
  the process has not joined the contract and nothing may be concluded from its
  traffic; an empty array means "I read nobody" and can be checked against what
  an observer sees. Do not drop the key to mean the same thing.

Neither loosens anything above: `/snapshot` still requires the same bearer
token, `collector.id` and `collector.kind` are still fixed strings, and the
control surface is still unreachable through it. `tests/snapshot_auth.sh` pins
all of that against the built binary — the handler is inline in `main()` behind
a real socket, so a unit test cannot reach it, and these properties had nothing
holding them before.

### D19 (revised) — not a blocker; the derivation closes on v1 fields · Settled

I called this a blocker for Phase 2 and overstated it. Re-reading the schema
before implementing the fix showed the fix is not needed.

The proposal was a new `load_ms_source` field distinguishing engine-reported
from derived load times. It is redundant. v1 already encodes the distinction in
`load_ms` itself: `null` means unmeasured, `0` means the model was resident, and
a positive value means the engine reported it. A reader can tell the three apart
without being told.

And load time *is* recoverable from a v1 trace without any new field, because
`ttft_ms` on a cold start is load plus prefill and the two separate cleanly:

1. Learn `prefill_rate` from **warm** records only, where `load_ms == 0` by
   definition and `ttft` is therefore prefill alone.
2. Subtract that prefill from `ttft` on **cold** records to get load.

Step 1 draws only on records where load is known to be zero, so step 2 never
feeds itself — no circularity. Both steps filter on `inflight_at_dispatch` for
the same reason the prefill caveat gives: a rate measured while the engine was
queueing is not a rate.

Written into `docs/TRACE-SCHEMA.md` under Derived quantities, so Phase 2
implements the bootstrap rather than rediscovering it.

What remains true from the original entry: the OpenAI-compatible path reports no
load duration, engine-reported values are better than derived ones, and Phase 2
should weight them accordingly. What was wrong: that this stopped Phase 2 from
starting. It does not. Phase 2 is unblocked.

### D23 — `footprint_bytes` added to the trace · Settled

Phase 2 learns load bandwidth as `footprint / load_ms`. A v1 record carried
`load_ms` but nothing to divide it by: the footprint the router used at
admission appeared nowhere, and no combination of the other fields reconstructs
it — `vram_free_at_dispatch` is the node's free memory, not the model's size,
and the candidate entries carry scheduling state rather than hardware.

So `footprint_bytes` is recorded. Rule 2 permits adding a field without a
version bump: readers that do not know it are unaffected, and readers that do
treat its absence as unknown rather than as zero.

The alternative was to have the learned model remember the footprints it
computed during `estimate()` and look them up in `observe()`. That was written
first and thrown away: `estimate` is `const` and runs under a shared lock, so a
cache written there is a data race, and a value that exists only in memory is
lost on restart — which is exactly when the trace replay is supposed to bring
the model back.

### D24 — What Phase 2 cannot learn, and says so · Settled

`LearnedCostModel::footprint_bytes` stays seeded. A v1 trace records the
footprint the router *expected*, not the VRAM the load actually consumed, so
there is nothing to correct the estimate against — `vram_free_at_dispatch` is a
single reading before the load, with no counterpart after it.

This is left visible rather than papered over. The footprint estimate is an
admission input, so an error there is a routing failure rather than a latency
one (D2), and claiming to have learned it would be the easiest lie available in
this file. Closing it needs a second VRAM reading after the load settles, which
is a schema and an agent change, and belongs to whichever phase actually needs
the accuracy.

**Superseded in part by D35.** The last paragraph was wrong about the data. It
is true that the *trace* carries no post-load reading, but the live node state
does: Ollama reports `size_vram` per resident model, the agent has always
forwarded it as `models_resident[].vram_bytes`, and admission was already
trusting it to price an eviction. So the measurement existed and was being used
on one side of the same question while the other side guessed. No schema change
was needed — only noticing.

### D25 — The router replays its trace on start · Settled

§4.2 said the router "rebuilds it from the trace log on start" and nothing did.
Phase 1 did not notice because a static model has nothing to rebuild.

`--replay_from` is deliberately separate from `--trace`. Defaulting to the trace
is right for a restart, but the Phase 2 benchmark needs a learned model with a
past that is not the run being measured — otherwise the arm improves *during*
the measurement and the number describes the warm-up rather than the model.

The ledger is not replayed. It accounts for requests in flight now, and nothing
from a previous process is.

### D26 — Placement's eviction rule is comparative, not a staleness cutoff · Settled

The first version evicted a resident model only if nothing had asked for it in
`stale_ms` (300 s by default). Measurement showed that makes the manager inert
in exactly the situation it exists for: during a busy period nothing is ever
stale, so no eviction is ever permitted, and under memory pressure every preload
needs room. Zero preloads across every run.

The rule is now relative — displace the resident with the least demand, and only
when the arrival is `evict_margin` times hotter (1.5 by default). That
comparison *is* Phase 3's claim, that frequency beats the engine's recency; an
absolute cutoff never made that claim at all. The margin stops two
equally-wanted models swapping places forever, each swap paying a load for
nothing.

### D27 — Eviction is instant, so there is nothing to pre-evict · Settled

§9 says "keep frequently-requested models resident, evict the stale", which
reads as two jobs. It is one. Dropping a model from VRAM costs no measurable
time, so evicting a stale model early saves nothing — the next request would
have evicted it just as fast at dispatch. All the value is in *preloading*, and
eviction appears only as the means of making room for one.

The manager therefore has no background tidy pass. It evicts when, and only
when, something better wants the space.

### D28 — `resident()` dropped from IEngineAdapter · Settled

§5 gave the adapter a method to list resident models. NodeState already carries
that, polled by the agent, so the method would be a second path to the same fact
— and two paths to one fact is one more thing that can disagree. The adapter is
now the write side only: load, drop.

Placement calls also go to the node's dispatch endpoint, which is the agent
(D18), not to the engine directly. The agent already proxies these paths for
inference; opening a second route to a port that is meant to stay on loopback
would undo D18 for no gain.

### D29 — An uncapped reply gets a length drawn from the model, not a constant · Settled

Phase 2 shipped with output-length learning never once exercised: every request
the load generator sent carried `max_tokens`, a cap beats anything inferred, so
both arms returned the cap and the learned EWMA sat unused. Measuring the
uncapped case means the simulated node has to decide the length itself.

A constant would have been worse than not testing. An EWMA converges on a
constant instantly and with zero error, so the learned arm would "win" against
a thing that does not exist. The node instead draws uniformly from a per-model
range, and the ranges are the ones the capped scenario was already using
— planner 110-170, sub-agent 40-90 — so the workload is unchanged in
distribution and only *who states the length* is different.

The draw is a hash of (node seed, model, prompt) rather than a step of a shared
RNG. Sub-agent calls arrive concurrently, so a shared stream would hand out
lengths in thread-completion order and the scenario would stop replaying
identically (D12). Hashing the request also makes the length a property of the
request, which is what it is in reality.

### D30 — The trace records the model's prediction, not the caller's cap · Settled

`predicted_output_tokens` was written from `RequestFeatures`, which holds the
caller's `max_tokens`. With a cap present the two agree exactly, so nothing
looked wrong for the whole of Phase 2. Uncapped, the field was **zero**, and a
zero is indistinguishable from "no prediction": the length error became
unmeasurable, and `LearnedCostModel::observe` skipped its error EWMA entirely,
leaving sigma pinned to a guessed 50% of the mean forever.

So the loop was open in exactly the case it exists for. It could learn the mean
and never learn how wrong the mean was.

`Estimate` now carries the prediction it priced, and dispatch reads it off the
winning candidate, falling back to the caller's cap only when there is no
winner. Capped records are byte-identical to before, so no existing result
moves and the schema keeps its version — the field's meaning was always
"from the cost model"; the code just was not doing it.

This was found by running the measurement, not by reading the code. The capped
campaign could not have exposed it: with a cap, predicted equals actual equals
cap, and every number in the panel looks perfect.

### D31 — Sigma is the model's own residual, not the one in the record · Settled

`observe()` measured output-length error as |actual − `predicted_output_tokens`|,
taking the prediction from the record. But a record's prediction belongs to
whichever model wrote it, and the learned arm is deliberately fed a trace
written by the static one (D25) so it never learns from its own measurement.
The static model predicts a flat 256 tokens; the replies are 40-90. So the
learned model opened every uncapped run believing its own error was ~190
tokens, when the mean it had learned was within ~3.

The measured effect, before the fix: sigma started at 192 tokens and decayed
only to 50 across a run, 122 of 125 decisions came back `within_noise`, and
97% of jobs landed inside a band claiming to be 1-sigma.

`within_noise` is a label, not a fallback — the policy still picks the minimum
— so routing was unharmed, and the corrected campaign confirms it: wall-clock
moved 74.42 s to 74.15 s, inside variance. The harm was to what the system
says about itself. A console reporting that 98% of its choices are
indistinguishable from noise, on a workload it actually fits to 16%, is
describing a scheduler nobody should trust, and §6.2 spends that band on real
comparisons. After the fix: 59 of 125, matching the capped campaign exactly,
and coverage of 48% — now somewhat *under* the 68% a 1-sigma band claims, which
is stated here rather than tuned away.

The residual is now measured against this model's own prior, read before the
mean is updated. That is what "how wrong am I" means, it is well-defined on a
trace from any source, and it no longer depends on a field the writer may not
have filled.

Capped predictions are untouched: with a cap, `predict_output` returns the cap
before it ever reaches the error EWMA, so no Phase 2 result moves.

### D32 — The Phase 3 campaign measured the wrong scenario · Settled

`bench/phase3.sh` ran the agent workload from Phases 1 and 2 and I reported it
as the pressure workload. Two independent bugs, either of which alone would have
done it:

- `start_agents()` launched `sim-desktop.json`, `sim-jetson.json` and
  `sim-laptop.json`. `pressure-a.json` and `pressure-b.json` were written,
  committed, and never started.
- The `loadgen.py` call omitted `--scenario pressure`, so the default agent
  scenario ran.

It survived review because `nodes.json` labelled ports 8981 and 8982
`pressure-a` and `pressure-b`. Every trace therefore carried pressure node
*names* over agent *models*, and every filename said pressure. What gave it away
was the field the load generator writes about itself: `"scenario": "agent"` on
all ten runs — recorded because a result labelled with the scenario we *meant*
to run would lie silently, which is the same reasoning that put `policy` in
`RESULT` back in Phase 1.

The harness now calls `verify_cluster` before measuring anything, and refuses to
run if the models it can see are not the pressure set. Checked in both
directions: it catches the Phase 1/2 profile started under a pressure node name,
and it passes the real pressure cluster. A guard that refuses everything is not
a guard.

The lesson is not "read the script more carefully". It is that a benchmark
should assert the world it thinks it is in, because the file names, the node
names and the output paths all agreed with each other and all of them were
wrong.

### D34 — Placement is worth having, but only where traffic leaves gaps · Superseded in part by D38

**The second half of this does not transfer to real hardware.** See D38 and
`docs/REAL-PLACEMENT-RESULTS.md`: given the same gaps, a real engine with a
resident-model limit turns every preload into a swap, and placement moves cold
starts instead of removing them. What follows is the simulated measurement,
which stands for a node that can hold more than one model.

Phase 3 failed its exit criterion on back-to-back traffic and met it on the same
scenario with 8 s pauses between turns: cold starts 9 → 8, wall-clock −6.3% at
25/25 pairwise, p95 flat, one preload per run. The cold starts it removes are
the repeat-after-eviction kind, which is the only kind a preload can remove.

So placement is not redundant and not essential. It is conditional, and the
condition is measurable: does the target node go idle for longer than the model
takes to load? `bench/predictive_ceiling.py` answers that on any trace, and the
answer swung from 0 of 45 hideable to 5 of 45 purely by adding think time.

It therefore stays **off by default** as a judgement rather than a hedge. Where
traffic is saturated the skip-when-busy rule makes it inert rather than harmful,
so the cost of leaving it on is zero; where traffic is interactive it pays. The
default is set for the case an operator is more likely to be measuring than
running.

A defect found while measuring this, and fixed before the numbers were reported:
`think()` drew its jitter from the RNG that picks models, so enabling think time
also changed the request sequence. Both arms still shared it, so the comparison
within a campaign held, but the comparison *across* think values moved two
variables at once — which is what D12 exists to prevent. The pause has its own
stream now, and the sequence is asserted identical at every think value rather
than assumed.

### D33 — Predictive placement: measured, declined, closed · Settled

§9 defers predictive placement to Phase 4 and declines to commit to it. D17 says
to reconsider with Phase 3's trace in hand. Reconsidered, on the corrected
pressure campaign, and **not built**.

Two conditions have to hold. Only one does.

**The next model is predictable.** A first-order Markov predictor scores 63.7%
against a 55.6% base rate over six models, and conditional entropy falls from
2.00 to 1.27 bits. D17's stated doubt — that a single cluster accumulates
transition data too slowly — does not hold here.

**There is no time to act on it.** Half the pressure scenario's wall-clock is
model loading, and an oracle predictor recovers none of it: 0 of 45 cold starts
could have been hidden. The largest idle lead anywhere in the campaign is
1,256 ms; the shortest load actually measured is 1,601 ms. That comparison uses
only measured values and needs no model.

Two independent measurements agree — the live manager skipped every cycle for
lack of an idle node, and a trace replay done afterwards by different means
found the same absence.

Prediction changes *when you decide*, not *whether there is a window to act in*.
So this is a decision rather than another deferral: building it would produce a
second component that is provably correct and provably never runs.

The strongest argument against this was that both benchmarks are closed loops,
which is an artefact of the harness rather than of local inference. So it was
tested rather than argued: with 8 s gaps the ceiling rises from 0 of 45 hideable
cold starts to 5, and **reactive placement takes all five** (D34).

That closes the objection in the decision's favour. On the workload where
predictive placement finally has room, something simpler already fills it, and
prediction would compete for zero remaining headroom. The argument improved from
"there is no window" to "there is a window and it is already occupied".

What is still open is spare capacity — a node idle because nothing needs it,
which two busy nodes cannot pose. `bench/predictive_ceiling.py` answers the
question on any trace, so reopening this stays a measurement and not an
argument.

### D35 — A resident model's real footprint prices a load onto a node without it · Settled

D24 said `footprint_bytes` could not be learned. It was half right, and the half
it got wrong was the half that mattered.

`seed_footprint_bytes` already returns the measured size when *this* node holds
the model. What stayed a guess was the case admission actually turns on: what a
load will cost on a node that does **not** hold it, while another node does.
That guess was `disk_bytes × 1.08 + kv(num_ctx)`.

Measured on the card itself — an RTX 4060 Laptop, 8188 MiB, running
`qwen2.5:7b-instruct-q4_K_M` under Ollama:

```
disk size          4.683 GB
size_vram          4.924 GB     ratio 1.0515, fully on the GPU
seeded predict     5.204 GB     disk x 1.08 + kv(4096)
seed error           +5.7 %     +280 MB
```

**This corrects the figure this decision was first written with.** It quoted a
ratio of 1.014 and a 456 MB error, taken from a measurement recorded earlier in
the project under a different Ollama build. Re-measured on the live card the
ratio is 1.0515 and the error is 280 MB — the seed still overshoots, but by
about half as much as claimed. The direction is what the decision rests on and
that is unchanged; the magnitude was restated rather than left standing.

280 MB per model on an 8 GB card is still worth having: it is the difference
between two models fitting and one, in the cases that sit near the boundary.

The engine reports the real number, so the overhead over disk size is learned
from any node that holds the model and applied to nodes that do not. It is
learned as a **ratio** rather than a byte count, because model weights are the
same bytes on every GPU and a ratio transfers where an absolute does not.

Two guards, both in the direction that matters:

- The observed size already contains the KV cache the engine allocated at its
  own default context, so only context asked for *beyond* that baseline is
  charged again. Under-estimating a footprint admits a node that cannot serve
  the request, which is a routing failure rather than a slow reply (D2).
- A ratio below 1.0 or above 4.0 is dropped rather than averaged. Below one is
  not a resident copy of these weights — a partial offload, or a name shared
  with a different quantisation — and averaging it would quietly shrink every
  later estimate. `tests/policy_test.cpp` covers this; the attempt to also
  demonstrate it on real hardware, by loading a 9.6 GB model onto the 8 GB card,
  took Docker's Linux engine down with it and was not repeated.

Harvesting happens in `estimate()`, which walks every candidate node anyway.
That method is `const` because scoring must not change a decision; recording an
observation is not a decision, so the store is `mutable` and nothing about the
ranking depends on it.

`static-v1` keeps the seeded formula. It is the baseline every phase is measured
against, and a baseline that improves alongside the thing it measures stops
being one.

### D36 — A completion outranks a poll that has not caught up · Settled

Found by running the router against a real GPU for the first time since Phase 0.
One node, an RTX 4060, Ollama in Docker. The first request loaded the model in
about 50 s. **The next three were refused with 503, `insufficient_vram`, for the
model already occupying the card.**

The chain explains it. NVML reports free VRAM instantly, so the load is visible
the moment it happens. Residency comes from Ollama's `/api/ps`, which is also
instant — measured at 0.01 s — but reaches the router through two caches: the
agent builds a snapshot on its own poll loop, and the router fetches that
snapshot on another. For up to two poll intervals the router therefore sees a
node whose VRAM is gone and whose resident list is empty, which reads exactly
like a full node that cannot take the model.

The router had better evidence and was not using it: **it had just served that
model on that node itself.** A completion is first-hand and needs no poll.

`NodeLedger` now remembers when it last finished serving each (node, model), and
`believed_loaded()` combines that with the engine's own answer. `node_stale_ms`
bounds the memory — the horizon the router already uses to decide a node's
state is too old to act on, rather than a second constant invented here.

Being wrong costs a reload, which is a slow reply. Being wrong the other way is
a 503 for a request the cluster could serve.

**One rule, two callers.** Admission and both cost models consult the same
function. The first fix only changed admission, and the real run showed why that
is not enough: requests were admitted, then priced as though they still had to
load — an 11 s estimate against a 0.7 s reply. On a multi-node cluster that is
the same defect wearing a quieter costume, since the router would rank the warm
node as the expensive one and route away from it.

Two wrong turns are worth recording, because both looked right:

- The first version compared the completion against `node.sampled_at_ms`. The
  router restamps that field with its own clock on receipt (§6.1, so a skewed
  node clock cannot look permanently fresh), so it marks when the state
  *arrived*, not when the engine was observed. The comparison was against the
  wrong instant and the bug survived unchanged.
- The second version stored the timestamps inside `NodeAccount`, which
  `release()` erases the moment a node goes idle so that `/admin/stats` lists
  only live nodes. The memory was deleted milliseconds after being written — at
  exactly the moment it becomes the only evidence anyone has. It lives beside
  the accounts now, not inside them.

Verified on the hardware that produced the failure: all four requests served,
`t_load` zero on the three that followed the load, and the load estimate no
longer appearing in their predictions.

### D37 — The router asks an OpenAI-shaped stream for its token counts · Settled

The real-hardware run left three fields null in every record: `output_tokens`,
`prompt_tokens_actual` and `load_ms`. Those are `observe()`'s inputs. Without
them the learned cost model has no decode rate, no prefill rate and no output
length to learn, so **against a real engine it silently degenerates into the
static model** — and every number Phase 2 reports depends on them arriving. The
simulated node always sent them, which is why five campaigns never noticed.

Measured against the engine rather than guessed:

```
OpenAI path, stream, no stream_options   no usage at all
OpenAI path, stream, include_usage       prompt_tokens 30, completion_tokens 8
native /api/chat, stream                 the same, plus load_duration
```

So the OpenAI shape carries no counts unless the caller asks, and Ollama follows
that faithfully. The Ollama-native shapes were never affected: they report
`eval_count` unprompted and the dispatcher already reads it.

**This breaks a stated invariant, so it is stated back.** `ingest.h` and
`dispatch.cpp` both said the body is proxied byte for byte and never rewritten.
It now has exactly one rewrite: `stream_options: {include_usage: true}`, added
only on the OpenAI shape, only when streaming, and never over a caller who set
`stream_options` themselves.

The client pays for it. Measured: seven chunks instead of six, the extra one
carrying usage and an empty `choices` array. That is the documented OpenAI
shape, but this client did not ask for it, and a client that assumes
`choices[0]` exists on every chunk will break on it. `dispatch.request_usage
false` restores the byte-for-byte proxy — verified in both directions — at the
cost of the cost model learning nothing from that path.

`load_ms` stays null here. Only the native path reports `load_duration`, and
reaching it would mean translating between the two APIs rather than proxying,
which is a larger change than this one and against §2 and D18. D19's bootstrap
already recovers a load time from ttft, so the gap is bounded and handled.

What this bought, on the card: `prompt_tokens_actual` arriving as 36 against an
estimated 33 — D15's estimator running about 8% low on short prompts, which
until now was unmeasurable outside simulation.

### D38 — An engine's resident-model limit is missing from the node state · Open

D34 said placement is conditional: inert on back-to-back traffic, worth having
where the workload leaves gaps. Both halves were measured on simulated nodes.
On two real engines the first half holds exactly and **the second does not**.

With 8 s gaps the manager gets its window and uses it — six or seven preloads a
run against zero back-to-back — removes exactly one cold start, and loses on
every latency line. Its own counters say **6-7 preloads, 0 evictions**, which is
where the answer is.

`OLLAMA_MAX_LOADED_MODELS=1` means the engine keeps one model. Loading `qwen`
throws `tinyllama` out, the engine does it implicitly, and the manager never
sees it. Measured: placement warmed one model on the GPU (19 of 19 cold down to
12 of 23) and cooled the other by the same amount (19 of 40 up to 25 of 40). It
did not remove cold starts. It moved them.

**The gap is in the contract, not the reasoning.** `NodeState` carries
`engine_slots`, which is *parallel requests the engine accepts*, and nothing
about how many models it will keep resident. Placement decides on
`vram_free_bytes` alone; the GPU reports ~7 GB free against two 400 MB models,
so the router believes both fit. Ollama disagrees and has no way to say so.

**A simulated node cannot contain this defect**, because its residency capacity
*is* its VRAM by construction. `bench/profiles/pressure-*.json` gave each node
room for two, so a preload there genuinely added one. That is the whole
difference between the simulated verdict and the hardware.

Left open rather than fixed, because the fix is a design choice and this entry
is the measurement:

- Ollama does not expose `OLLAMA_MAX_LOADED_MODELS` over its API, so the agent
  cannot simply report it.
- It is inferable: a preload that consistently displaces another model on the
  same node says the capacity is one. That is learnable the way everything else
  here is learned, and it belongs to whichever phase needs it.
- Until then the manager cannot distinguish "preload B" from "swap A for B",
  and those have opposite value.

One smaller lesson, recorded because it cost a campaign: the manager's
`evictions` counter reads 0 throughout. It is accurate about what the manager
did and silent about what happened, and a counter that only sees its own actions
is not measuring the system.
