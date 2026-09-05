# Phase 3, re-run on the scenario it was written for

The first Phase 3 campaign measured the agent workload under Phase 3 labels and
I reported it as the pressure workload. `docs/PHASE3-RESULTS.md` carries the
retraction and the two harness bugs behind it. This is the corrected campaign:
six models on two nodes that hold two each, accessed with a skew, which is what
`bench/phase3.sh` always said it was doing.

**Verdict: the exit criterion is still not met — for a completely different
reason, and a more useful one.**

## Numbers

| | LRU (placement off) | placement on |
| --- | ---: | ---: |
| **cold starts per run** | **9** | **9** |
| — first touch | 4 | 4 |
| — repeat after eviction | 5 | 5 |
| **preloads performed** | — | **0** |
| **cycles skipped: node busy** | — | **24-26 per run** |
| wall-clock median (s) | 61.45 | 61.69 |
| — spread | 61.3-61.7 | 61.5-65.0 |
| — pairwise | | 4/25, inside variance |
| total p50 / p95 (ms) | 1,680 / 4,963 | 1,709 / 4,972 |
| ttft p50 / p95 (ms) | 342 / 3,438 | 352 / 3,467 |

Five runs each. Placement is marginally worse on every latency line and 4/25 on
wall-clock, all of it inside run-to-run variance. Nothing improved.

## The manager was starved, not satisfied

The retracted report said the manager found nothing to do because every model
with demand was already resident. That was the agent workload's answer. Under
real pressure the answer is the opposite: **there is plenty to do and never a
moment to do it in.**

```
placement: 0 preloads, 0 evictions, 0 failures, 24 cycles skipped (busy)
```

Twenty-four to twenty-six skips per run, out of a cycle every 2 s across a 61 s
run — that is *every* cycle. The rule doing the skipping is right:

> A preload occupies a slot. Doing it while the node is working steals capacity
> from the requests placement exists to help, and shows up as the p95 regression
> the exit criterion forbids.

So the manager is correctly refusing to make things worse. But a workload that
keeps the engine busy leaves no window at all, and a reactive manager with no
window is a component that cannot run.

**This is measurable independently of the manager.** Taking the LRU arm's traces
and asking how much idle time existed on the target node between the previous
request arriving and the cold one dispatching (`bench/predictive_ceiling.py`):

```
cold starts                45  (9.0 per run)
load time to hide             156.1 s   (50.8% of all wall-clock)
idle lead available, p50          3 ms   vs load p50 3726 ms
fully hidden                   0 / 45
```

Half the wall-clock of this scenario is spent loading models, and **none of it
was reachable**: 30 of the 45 cold starts had 1-3 ms of idle lead, and the other
15 had about 1.2 s against a load of several seconds.

The strongest form of that statement does not depend on any estimate. The
largest idle lead anywhere in the campaign is **1,256 ms**. The shortest load
actually measured is **1,601 ms**. No cold start in this campaign could have
been fully hidden by anything, at any lead, under any load figure consistent
with the data.

## What placement would need

Not a better rule — a different workload. Specifically, idle capacity on the
node that will serve the next request, for longer than that model takes to
load. Two situations plausibly provide it and neither is in this scenario:

- **A cluster with spare nodes.** Preloading onto a node that is idle *because*
  it is not being used costs nothing. This scenario has two nodes and keeps both
  busy; a third idle node changes the arithmetic entirely.
- **Bursty traffic with real gaps.** A workload that goes quiet for tens of
  seconds between bursts hands the manager exactly the window it needs. This
  scenario is back-to-back by construction.

Both are untested, and the second is the more interesting: it is what an
interactive agent session actually looks like between a human's turns.

## Honest limits of the measurement

- **Two-thirds of the load costs are the router's own estimate.** The engine
  reports no load duration on the chat path, so `load_ms` is null and the cost
  is recovered from ttft with prefill subtracted (D19's bootstrap). That needs a
  warm baseline for the same (node, model), and 30 of 45 cold starts are for
  pairs that were never warm. Where both exist, measurement runs about 22% below
  the estimate — 3,058 ms against 3,726 — so the estimate is the right size and
  errs toward *overstating* the load. Correcting it does not change the verdict,
  because the max-lead-versus-min-load argument above uses only measured values.
- **Simulated nodes.** Load bandwidth comes from `bench/profiles/pressure-*.json`.
  What transfers is the structure — the ratio of load time to idle time — not
  the seconds.
- **One skew, one seed.** `PRESSURE_HOT_SHARE` is 0.5 with the cold models
  rotating. A heavier skew leaves more of the working set resident and gives
  placement less to do, not more.

## Phase 3 exit checklist

| §9 requirement | state |
| --- | --- |
| `PlacementManager` using `IEngineAdapter::preload` / `evict` | done |
| Reactive: keep frequently-requested models resident, evict the stale | done (D27) |
| Compared against plain LRU | done, five runs, on the intended scenario |
| **Exit criterion:** cold starts drop substantially, no p95 regression | **not met** — 9 vs 9; the manager never acted |

Placement stays implemented and **off by default**. The machinery is verified
against a simulated node (load, then evict, both through the adapter), the
skip-when-busy rule is correct and is what prevents the p95 regression, and the
component is ready for a workload that leaves it room. This one does not.
