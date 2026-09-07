# Phase 1 result

**Verdict: the exit criterion is met. Warmth beats RoundRobin by 7.8% on
scenario wall-clock, and the run-to-run spreads do not overlap.**

It also makes p95 latency worse, which is a real cost and is reported here
rather than buried. The section on attribution is the one that matters most: a
wall-clock win does not by itself establish that the win came from *warmth*.

---

## What was run

Five runs per policy, identical scenario, identical seed. Each run is 5 rounds
of one planner call on a 7 GB model plus four concurrent sub-agent calls on a
2 GB model — the shape §9 asks for, because a single-model workload has no
warmth to be aware of and a workload without concurrency never exercises the
queue or contention terms.

The cluster is three simulated nodes (D16), sized so the question is genuinely
open rather than rigged:

| node | VRAM | decode (planner / worker) | holds both models? |
| --- | --- | --- | --- |
| `sim-desktop` | 8 GB | 30 / 90 tok/s | **no** — 7 + 2 > 8, so it thrashes |
| `sim-jetson` | 15 GB | 14 / 42 tok/s | yes, and starts warm |
| `sim-laptop` | 6 GB | — / 34 tok/s | planner does not fit at all |

The simulated agents are restarted between runs so residency resets; otherwise
run 2 inherits run 1's warm cache and the comparison measures ordering. Hardware
seeds in `bench/router.json` are deliberately within about 10% of the rates
those nodes actually deliver, in both directions — exact seeds would hand the
cost model the answer.

Reproduce with:

```bash
bench/compare.sh --runs 5 --rounds 5
python3 bench/summarize.py bench/results
```

## Numbers

```
                             roundrobin-v1     warmth-v1
  PRIMARY — what Phase 1 is decided on (D12)
  wall-clock median (s)              83.54         77.06     +7.8%  better
    spread across runs           83.1-84.5     75.6-77.4
    runs                                 5             5

  REPORTED — can move against wall-clock; not decisive
  total p50 (ms)                     2,925         2,299    +21.4%  better
  total p95 (ms)                    13,732        14,600     -6.3%  WORSE
  ttft p50 (ms)                      2,176         1,273    +41.5%  better
  ttft p95 (ms)                      8,676         3,326    +61.7%  better
  cold starts per run                   12             3    +75.0%  better
  timing error (%)                    10.0           9.4     +6.4%  better
```

## The p95 regression is real

D12 chose wall-clock as the deciding metric precisely because "routing to a warm
weak node can improve p50 while damaging p95". That is what happened, and it
would have been easy not to notice if the criterion had been "is it faster".

The mechanism is visible in the routing. Warmth concentrates work on two nodes
(desktop 14, jetson 10, laptop 1 per run) where RoundRobin spreads it over three
(11 / 8 / 6). Concentration is what produces the wall-clock and median wins —
the work lands where it runs fastest and where the model is already loaded — and
it is also what deepens the queues on those two nodes, which is where the worst
case gets worse.

So the honest statement is: **Warmth finishes the batch sooner and answers the
typical request sooner, at the cost of a slightly worse worst case.** For an
agent workload, where the question is when all the sub-tasks are done, that is
the right trade. For an interactive workload with a latency SLO it might not be,
and nothing here establishes that it would be.

TTFT moves the other way and moves a long way: p95 time-to-first-token drops from
8.7 s to 3.3 s, because a request that would have waited for a cold load now
usually does not.

## Attribution: how much of this is actually warmth?

This is the part that keeps the verdict honest. Warmth also declines to use the
weak node, and **any** speed-aware policy would have found that — so some of the
7.8% is not about model residency at all.

Every trace record carries the full per-candidate breakdown (D10), so the
question is answerable from the traces already collected, with no extra runs.
Replaying each decision with the `t_load` term removed — keeping every other
term — gives:

- **Dropping `t_load` would change 36 of 125 Warmth decisions (29%).**
- The spread of `t_load` across admitted candidates has a **p50 of 0 ms**: in
  half the decisions every candidate was equally warm and there was no warmth
  signal to act on. Its p95 is 8,890 ms, so where the signal exists it is large.
- Warmth's own attribution agrees: `t_load` was the deciding term in 23 of 125
  decisions, `t_decode` in 20, `t_queue` in 18.

So roughly **three in ten decisions are warmth-driven**, and the rest of the win
comes from the cost model seeing node speed and queue depth. The cold-start count
falling from 12 to 3 per run is the mechanism by which those three-in-ten pay off.

That is a smaller claim than "warmth-aware routing is 7.8% faster", and it is the
one the data supports. It is also enough: the whole premise of §1 is that
`T_load` belongs in the same units as the other terms so it can be *compared*
against them — not that it dominates them.

## Half the decisions are inside the noise band

> **Superseded in part by D39.** Every `within_noise` count on this page was
> computed with a band that has since been measured and found 7-12× too narrow
> in the cold regime — so the real number of coin-flip decisions is *higher*
> than reported here, not lower. Wall-clock, cold starts and every other figure
> are unaffected: sigma has never influenced routing, only its description.
> See `docs/UNCERTAINTY.md`.

**64 of 125 Warmth decisions were recorded as `within_noise`** — the margin
between the best two candidates was smaller than the uncertainty on either.

This is the design working, not failing (D8). Output length is unknown before a
request and drives the largest term, so a ±35% band on `T_decode` really does
swallow a sub-second margin. A scheduler that reported a confident `decided_by`
on those 64 would be claiming precision it does not have.

It does bound what Phase 1 can claim: the win rides on the 61 decisions that were
outside the band. It is also the clearest argument for Phase 2 that the data has
produced — a better output-length prediction narrows sigma, which converts
noise-band decisions into real ones.

## What this does not show

- **The magnitude is not a real-world number.** These are simulated nodes with
  rates chosen to be defensible, not measured from the hardware they imitate.
  What transfers is the pipeline, the policy logic and the sign of the result.
- **One scenario, one seed.** The workload shape was fixed in advance and not
  tuned after seeing results, but it is still a single shape. A workload with
  one model, or with no VRAM pressure, would show much less — by construction,
  since there would be nothing for `T_load` to distinguish.
- **`load_ms` is null on every cold start** (D19). The simulated node reproduces
  the real engine's behaviour of not reporting load duration on the
  OpenAI-compatible path. Phase 2 learns load bandwidth from that field, so it
  has to be fixed before Phase 2 can begin.

## Phase 1 exit checklist

| §9 requirement | state |
| --- | --- |
| `StaticCostModel` with seed tables, footprint estimation, ledger-aware `T_load` | done |
| `Warmth` policy | done |
| Runtime policy switching (`POST /admin/policy`) | done |
| Reproducible load generator | done (`bench/loadgen.py`, seeded) |
| Report script: wall-clock, p50/p95, cold starts, prediction error | done (`bench/trace_report.py`, `bench/summarize.py`) |
| Minimal UI: nodes, telemetry, job feed, decision panel | **outstanding** — being designed separately against `docs/UI-BRIEF.md` |
| **Exit criterion:** Warmth beats RoundRobin on wall-clock, median of 5, IQR reported, ≥3 nodes | **met** |

The UI is the one Phase 1 deliverable not built here. It does not gate the
measurement — the decision breakdown it renders is already in the trace, and
`bench/trace_report.py` reads it — but §4.3 is not satisfied until it exists.
