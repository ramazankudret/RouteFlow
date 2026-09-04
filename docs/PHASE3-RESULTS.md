# Phase 3 result

**Verdict: the exit criterion is not met.** §9 asks for the cold-start count to
drop substantially against LRU. It did not drop at all — 4 per run in both arms,
identical in all five runs, with zero preloads performed.

This is a negative result, not a broken build. The placement manager works; it
correctly decides there is nothing to do. The reason is worth more than the
feature would have been.

## Numbers

| | LRU (placement off) | placement on |
| --- | ---: | ---: |
| **cold starts per run** | **4** | **4** |
| — first touch | 2 | 2 |
| — repeat after eviction | 2 | 2 |
| preloads performed | — | **0** |
| wall-clock median (s) | 78.41 | 78.29 |
| total p95 (ms) | 14,557 | 14,526 |
| ttft p95 (ms) | 3,328 | 3,327 |

Five runs each. The wall-clock difference is 17/25 pairwise, which is inside
run-to-run variance. The p95 guard is satisfied — placement did no harm — but
that is what doing nothing looks like, not a result.

Every single run produced the same split: 2 first-touch, 2 repeat, 21 warm.

## Why there was nothing to do

The manager logs its reasoning. Every cycle on an idle node came back the same
way:

```
placement pressure-a: idle, wanted [hot:4b=13 (resident), cold-b:4b=2 (resident)], nothing to preload
placement pressure-b: idle, wanted [cold-a:4b=2 (resident)], nothing to preload
```

Every model with demand was already resident on the node that wanted it.

**A warmth-aware router does the placement as a side effect.** Warmth sends each
request to the node holding the model warm; that concentrates demand exactly
where residency already is; which keeps residency where demand is. The loop
closes on itself, and a reactive manager watching the same signal arrives
after the fact with nothing left to fix.

LRU is not being beaten here because LRU is not losing. Under a skewed workload
the most-frequently-used model is usually also the most-recently-used one, so
recency and frequency agree, and the engine's own eviction is already doing what
a frequency-aware manager would have done.

## The mechanism works — that was checked separately

A feature that never acts could be a feature that *cannot* act. Verified
directly against a simulated node, outside the manager:

```
resident at start:      ['hot:4b']
preload cold-c:4b:      3.01 s load  →  ['hot:4b', 'cold-c:4b']
evict hot:4b:                        →  ['cold-c:4b']
```

Load and unload both work through the adapter's path — Ollama's
`/api/generate` with an empty prompt and a `keep_alive`. So the manager is
correct and inert, not broken. That distinction is the difference between a
negative result and an untested one.

## Two design errors the measurement found

Neither would have been visible by reading the code.

**An absolute staleness cutoff makes the manager inert under pressure (D26).**
The first version evicted only models nothing had asked for in `stale_ms`
(300 s). During a busy period nothing is ever stale, so no eviction is ever
permitted — and under memory pressure every preload needs room. Zero actions,
always. The rule is now comparative: displace the least-wanted resident when the
arrival is 1.5× hotter. That comparison *is* Phase 3's claim; an absolute cutoff
never made the claim at all.

**The first "pressure" scenario had no pressure.** Four models of 3 GB on two
8 GB nodes holding two each is exactly four model-slots for four models — the
whole working set fits and nothing ever has to give way. Corrected to six
models over four slots.

Both were fixed before the campaign, and the result above is from the corrected
version. Neither fix changed the outcome.

## What this does not show

- **Placement may still matter where the router cannot route to warmth.** If the
  warm node is saturated and requests must spill to a cold one, or a node
  restarts empty while traffic is already flowing, the feedback loop above is
  broken and a manager has something to do. Neither situation occurs in this
  scenario, and neither has been measured.
- **The scenario has two nodes and one hot model.** A larger cluster with
  several competing hot models, or a workload whose hot set shifts over time,
  would separate frequency from recency more sharply. That is the case where
  LRU should start losing, and it is untested.
- **This says nothing about predictive placement.** §9 deferred that to Phase 4
  and declined to commit to it. Nothing here argues for or against it; if
  anything, the result raises the bar, because the reactive half turned out to
  be redundant.

## Recommendation

Leave placement implemented and **off by default**, which is how it ships. It
costs nothing when disabled, the machinery is verified, and it is the natural
starting point if a workload turns up where the router genuinely cannot route to
warmth.

Do not enable it on the strength of the architecture's expectation. §9 named
LRU as the baseline precisely so this could be checked, and on this workload LRU
wins by not being worse.

## Phase 3 exit checklist

| §9 requirement | state |
| --- | --- |
| `PlacementManager` using `IEngineAdapter::preload` / `evict` | done |
| Reactive: keep frequently-requested models resident, evict the stale | done — with eviction as the means to a preload, never a background tidy (D27) |
| Compared against plain LRU | done, five runs |
| **Exit criterion:** cold starts drop substantially, no p95 regression | **not met** — cold starts identical (4 vs 4); p95 guard satisfied only because nothing happened |
