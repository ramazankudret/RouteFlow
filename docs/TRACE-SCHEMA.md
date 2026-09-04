# Trace schema

Append-only JSONL. One record per dispatch attempt. This file is the project's
primary asset: the cost model, the UI history and the evaluation harness all read
it.

**The format is a contract.** Rules, in force from the first record:

1. Every record carries `"v"`. Current version: **1**.
2. A field may be **added**. A field may never be **repurposed** or have its unit
   or type changed. To change meaning, add a new field and bump `v`.
3. A reader must ignore unknown fields and must handle a missing field added in a
   later version by falling back to a documented default.
4. A malformed line is **skipped and counted**, never fatal. The count is exposed
   at `GET /admin/stats` as `trace_parse_errors`.
5. All durations are **milliseconds**. All sizes are **bytes**. All timestamps are
   **ISO-8601 UTC with milliseconds**, e.g. `2026-09-03T14:21:07.412Z`.
6. A duration that is not applicable is `0`. A value that is **unknown** is
   `null`. These are different: `load_ms: 0` means the model was resident;
   `load_ms: null` means we failed to measure it.

---

## Fields (v1)

### Identity

| Field | Type | Notes |
| --- | --- | --- |
| `v` | int | Schema version. `1`. |
| `job_id` | string | ULID, monotonic within a process. |
| `retry_of` | string \| null | `job_id` of the attempt this one replaces (§6.4). |

### Timestamps

Four points, not one. Every duration below is derivable from them, and clock skew
between router and node stays debuggable.

| Field | Type | Notes |
| --- | --- | --- |
| `ts_received` | string | Request accepted by the router. |
| `ts_dispatched` | string | First byte written to the upstream node. |
| `ts_first_token` | string \| null | First content token seen from upstream. `null` if the job produced none. |
| `ts_done` | string | Stream closed, success or failure. |

### Request

| Field | Type | Notes |
| --- | --- | --- |
| `model` | string | As requested by the client, before any alias resolution. |
| `role_hint` | string \| null | Optional caller hint (§8). Never required. |
| `num_ctx` | int | Requested context window. `0` = engine default. |
| `stream` | bool | Whether the client asked for a stream. |
| `prompt_tokens_est` | int | RouteFlow's pre-dispatch estimate (D15). |
| `prompt_tokens_actual` | int \| null | Engine-reported. `null` if the engine did not report it. |
| `output_tokens` | int \| null | Engine-reported completion tokens. |

### Routing

| Field | Type | Notes |
| --- | --- | --- |
| `policy` | string | Active policy name, e.g. `roundrobin-v1`, `warmth-v1`. |
| `cost_model` | string | Active cost model name, e.g. `static-v1`. |
| `node_id` | string | Winner. Empty string if `outcome == no_candidate`. |
| `was_resident` | bool | Model was in the winner's VRAM at decision time. |
| `decided_by` | string | Term with the largest gap to the runner-up, or `within_noise` (D8), or `single_candidate`. |
| `margin_ms` | number \| null | Winner vs runner-up predicted total. `null` with one candidate. |
| `evicted` | array of string | Models evicted to make room. Usually empty. |

### Prediction

Recorded on **every** record. Prediction error is the cost model's own accuracy
metric and is never combined into a single number with timing error (§8).

| Field | Type | Notes |
| --- | --- | --- |
| `predicted_output_tokens` | int | |
| `predicted_output_sigma` | number | 1-sigma, tokens. |
| `predicted_total_ms` | number | Winner's `Estimate::total_ms()`. |
| `predicted_sigma_ms` | number | Winner's `Estimate::sigma_ms`. |

### Measurement

| Field | Type | Notes |
| --- | --- | --- |
| `queue_wait_ms` | number | **Router-side only**: `ts_dispatched - ts_received` (admission, scoring, connect). Queueing *inside* the engine is not separately observable and lands in `ttft_ms` — see the caveat under Derived quantities. |
| `load_ms` | number \| null | Time attributable to model load. `0` if the model was resident; `null` if the engine reported no load duration and we could not measure it. |
| `ttft_ms` | number \| null | `ts_first_token - ts_dispatched`. |
| `total_ms` | number | `ts_done - ts_dispatched`. Excludes router-side admission. |
| `inflight_at_dispatch` | int | **Ledger** value (D4), not telemetry. |
| `concurrent_decoders_at_dispatch` | int | Ledger value. Required by D5. |
| `gpu_util_at_dispatch` | number \| null | `null` when `telemetry_ok == false`. |
| `vram_free_at_dispatch` | int \| null | Same. |
| `telemetry_backend` | string | `nvml` \| `tegra` \| `null` \| `sim`. |

### Candidates

`candidates` is an array covering **every node considered**, admitted or not,
each with its own state at decision time. The losers' numbers are not optional:
counterfactual analysis ("would the other node actually have been faster?") is
how the Phase 1 result is defended, and it cannot be done from the winner alone.

| Field | Type | Notes |
| --- | --- | --- |
| `node_id` | string | |
| `admitted` | bool | |
| `reason` | string | `ok` \| `engine_down` \| `model_missing` \| `insufficient_vram` \| `excluded` \| `node_stale`. |
| `predicted_total_ms` | number \| null | `null` when not admitted. |
| `t_queue`, `t_load`, `t_prefill`, `t_decode`, `t_evict` | number \| null | Term breakdown. |
| `sigma_ms` | number \| null | |
| `conf` | string | `seeded` \| `learning` \| `converged`. |
| `omitted` | array of string | Terms dropped for lack of signal (D3). Never silently zero. |
| `vram_free` | int \| null | State at decision time. |
| `gpu_util` | number \| null | |
| `was_resident` | bool | |
| `inflight` | int | Ledger value. |

### Outcome

| Field | Type | Notes |
| --- | --- | --- |
| `outcome` | string | `ok` \| `no_candidate` \| `dispatch_failed` \| `stream_failed` \| `client_abort` \| `timeout`. |
| `error` | string \| null | Human-readable detail. `null` when `ok`. |

---

## Derived quantities

Phase 2 recovers every rate it needs from v1 fields, with no extra
instrumentation. This closure is a property of the schema and must survive any
future version.

```
t_prefill_actual = ttft_ms - load_ms - queue_wait_ms
t_decode_actual  = total_ms - ttft_ms

prefill_rate(node, model) = prompt_tokens_actual / t_prefill_actual
decode_rate(node, model)  = output_tokens / t_decode_actual
      ... but ONLY when concurrent_decoders_at_dispatch == 1.
      Contended records instead solve for alpha(node):
          alpha = (base_rate / observed_rate - 1) / (concurrent_decoders - 1)

load_bandwidth(node)      = footprint_observed / load_ms
footprint_observed        = vram_free_before - vram_free_after   (cold loads only)
```

**Recovering load time when the engine does not report it.** `load_ms` is null
whenever the engine reports no load duration — which, on Ollama's
OpenAI-compatible endpoint, is every cold start (D19). It does not follow that
load bandwidth is unlearnable from a v1 trace: `ttft_ms` on a cold start *is*
load plus prefill, and the two can be separated with a bootstrap that uses only
fields already present.

```
1. prefill_rate(node, model)  from WARM records only
       was_resident == true  and  inflight_at_dispatch < engine_slots
       prefill_rate = prompt_tokens_actual / (ttft_ms - queue_wait_ms)
   A warm record has load_ms == 0 by definition, so ttft is prefill alone.

2. load_ms_derived            for COLD records only
       was_resident == false and  inflight_at_dispatch == 0
       load_ms_derived = ttft_ms - queue_wait_ms
                       - prompt_tokens_actual / prefill_rate(node, model)

3. load_bandwidth(node) = footprint / load_ms  where load_ms is engine-reported,
   else footprint / load_ms_derived, weighted lower.
```

There is no circularity: step 1 draws only on records where load is known to be
zero, so step 2 never feeds itself. The `inflight_at_dispatch` filters matter in
both steps for the same reason as the caveat below — a rate measured while the
engine was queueing is not a rate.

A record where `load_ms` is a positive number came from the engine and is worth
more than a derived one. No extra field is needed to tell them apart: `null`
means unmeasured, `0` means the model was resident, and a positive value means
the engine said so.

**The prefill caveat.** `queue_wait_ms` measures only the router's own overhead.
Time a request spends queued *inside* the engine is invisible to us — no engine
reports it — so it lands inside `ttft_ms` and therefore inside
`t_prefill_actual`. A record taken while the node was already saturated will
attribute engine queue time to prefill and learn a prefill rate that is too
slow. Phase 2 must therefore learn `prefill_rate` **only from records with
`inflight_at_dispatch < engine_slots`**, exactly as it learns `decode_rate` only
from uncontended ones. Both filters exist for the same reason: a rate learned
under queueing is not a rate.

Records with `outcome != "ok"` are excluded from rate learning but **included** in
wall-clock and availability reporting. Records with `retry_of != null` are counted
once as latency (the successful attempt) and separately as a failure event.

---

## Version history

| `v` | Date | Change |
| --- | --- | --- |
| 1 | 2026-09-03 | Initial contract. Supersedes the unversioned rev 1 sketch, which lacked `v`, split timestamps, an `outcome` enum, ledger counters and per-candidate loser state. |
