# TTFT under Warmth: measured, and declined

Every real two-node campaign has reported the same thing, and reported it as a
loss: Warmth's time-to-first-token is materially worse than RoundRobin's. On the
current router, p50 goes 1,311 → 1,829 ms and p95 goes 2,660 → 4,033 ms, while
wall-clock goes the other way by 25.6%.

§9 makes wall-clock the criterion for agent workloads, where nobody watches an
intermediate reply. That is an assumption about the caller, not a fact about the
cluster, and it had never been tested against the alternative. This is the test.

## Why it happens

Warmth pays a load to get a faster decode. The load lands in front of the first
token; the decode saving is spread over the rest of the reply. So the first
token is late and the last one is early — which is the premise working, seen
from the wrong end.

Avoiding that load means routing to whichever node is already warm. On this
cluster the warm node is usually the CPU engine, which decodes eight to twenty
times slower.

## What a TTFT-aware objective would buy

`bench/ttft_frontier.py` scores `total + w * ttft` against the recorded
candidates — D10 keeps the losers' estimates precisely so a counterfactual like
this needs no new campaign. `w = 0` is what ships; `w → ∞` is minimise TTFT.

The two objectives disagree on **33 of 80** requests, so there is a real trade
to make. Where they differ, the model expects to save 947 ms of TTFT and pay
1,886 ms of total time: **2.16 ms of total for every 1 ms of first token.**

| w | predicted total (s) | ttft p50 | ttft p95 | vs `w = 0` |
| ---: | ---: | ---: | ---: | ---: |
| 0 (ships) | 169.3 | 1,374 | 2,805 | — |
| 0.5 | 170.4 | 1,374 | 2,769 | +1% |
| 2 | 186.5 | 1,374 | 2,498 | +10% |
| 8 | 208.3 | 1,374 | 2,190 | +23% |
| min-ttft | 273.3 | 1,374 | 1,803 | **+61%** |

**The median does not move at all.** No weight changes TTFT p50; only the tail
responds, and it responds expensively. The frontier is a straight line with a
bad slope, so there is no weight worth choosing — a knob here would only let an
operator slide along it.

## And it does not even serve the person waiting

The table above is the batch's cost. Someone watching their own reply cares
about a different number, so:

| | ttft p50 | **total p50** | **total p95** |
| --- | ---: | ---: | ---: |
| `w = 0` (ships) | 1,374 | 2,470 | 3,961 |
| min-ttft | 1,374 | 2,661 | **11,236** |

Chasing the first token does not deliver it any sooner at the median, and makes
the *complete* reply arrive nearly three times later at p95. There is no caller
this trade serves: not the batch, not the individual.

**Decision: do not build it.** No `ttft_weight`, no interactive policy. Closed
as measured-and-declined, the way Phase 4 was.

## The other cluster shape, measured

The paragraph that used to sit here said this finding depends on the warm node
being the *slow* one, that on comparable nodes the trade might be free rather
than bad, and that the substitute could not test it. Two engines on the same
card can: 305.2 against 303.8 tok/s, a ratio of 1.008
(`bench/real_cluster.sh ratio gpu`). Three models across two nodes, because with
two of each a warmth-aware router just gives every model a node and there is no
warmth question left.

| | warm node 8-20× slower | warm node comparable |
| --- | ---: | ---: |
| objectives disagree | 33 of 80 (41%) | **2 of 30 (7%)** |
| ttft saved where they differ (p50) | 947 ms | **0 ms** |
| cost of min-ttft, total time | +61% | **+1%** |
| ttft p50 under min-ttft | unchanged | unchanged |

**The guess was wrong in an instructive direction.** The trade is not free on
comparable nodes; it very nearly does not exist. The two objectives already
agree on 93% of requests, and on the handful where they differ, minimising TTFT
buys **zero milliseconds** at the median while costing time.

So the decision holds in both regimes, for opposite reasons: where the warm node
is much slower the trade is real and bad, and where it is comparable there is
almost nothing to trade. A `ttft_weight` would be harmful in one and pointless
in the other.

## What this does not show

- **The comparable pair shares one card.** Two engines on 8 GB and one SM array
  contend in a way two machines would not, and the estimator is noticeably worse
  there — predicted ttft p50 603 ms against 1,746 measured. The disagreement
  *rate* is a property of the ranking and survives that; the millisecond figures
  in the table above should be read as the model's opinion, not the clock's.
- **These are the model's estimates**, not clock readings for roads not taken.
  They are worth something here because the model's residency belief now agrees
  with the engine on all 80 requests, and it prices a load within 300 ms of the
  measured cost (2,332 predicted against 2,636 actual). It is enough to decide
  whether to build; it would not be enough to publish a win.
- **Interactive traffic on this cluster is badly served either way**, and that
  is a hardware answer rather than a scheduling one: a card that must swap
  between two models cannot give a low first token. More VRAM removes the
  problem that scheduling here can only move around.

## Reproduce

```bash
# the original cluster: a GPU and a CPU engine
python3 bench/ttft_frontier.py "real 2-node" 'bench/results-real-two-node/warmth-v1-run*.jsonl'

# two comparable nodes, three models
bench/real_cluster.sh ratio gpu
bench/real_cluster.sh ratio-campaign 10
```
