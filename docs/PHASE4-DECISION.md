# Phase 4 — predictive placement: the reconsideration, and the decision

ARCHITECTURE §9 defers predictive placement to Phase 4 and **declines to commit
to it**. D17 says to reconsider it with Phase 3's trace in hand, for the same
reason §8 refuses to reach for regression before the EWMA is measured.

Phase 3's trace is now in hand — the corrected one, on the workload placement
was supposed to need. This is the reconsideration.

**Decision: do not build it. Phase 4 is closed as measured-and-declined, not
deferred again.**

## The idea, and the two things it needs

Predictive placement learns which model tends to follow which and preloads
before the request arrives. For that to pay, two things must both be true:

1. **The next model must be predictable** from what the trace can see.
2. **There must be time to load it** before the request lands.

The second is the one nobody checks, and it is the one that decides here.

## 1. The next model is predictable — genuinely

Scoring a first-order Markov predictor against the base rate (always guess the
commonest model) over the corrected pressure traces:

```
6 distinct models, 124 transitions
base rate (always guess the commonest)   55.6 %
first-order Markov (guess from previous) 63.7 %
entropy: 2.00 bits marginal -> 1.27 bits given the previous model
```

Knowing the previous model removes about 0.73 bits of uncertainty and adds 8
points of top-1 accuracy. **D17's stated worry — that a single cluster
accumulates transition data too slowly to learn from — does not hold on this
workload.** The structure is there and 125 records is enough to see it.

On the agent workload there is nothing to learn, but that is arithmetic rather
than a finding: two models, and the base rate already scores 80.6%.

## 2. There is no time to act on it — and this is fatal

`bench/predictive_ceiling.py` replays every cold start and asks how much idle
time existed on the target node between the earliest a one-step predictor could
have fired and the moment the request dispatched. Every assumption is set in
predictive placement's favour: the predictor is an oracle that knows the next
model *and* the node it will land on, it fires on the previous request's
arrival, and idle time is summed across gaps even though a load does not pause
and resume.

| | agent workload | pressure workload |
| --- | ---: | ---: |
| cold starts | 15 | 45 |
| load time to hide | 28.9 s (7.8% of wall-clock) | 156.1 s (**50.8%**) |
| idle lead, p50 | 0 ms | 3 ms |
| load, p50 | 1,912 ms | 3,726 ms |
| **fully hidden** | **2 / 15** | **0 / 45** |
| ceiling on wall-clock | 0.8% | 5.9% (partial overlap only) |

On the pressure workload, half of all wall-clock is spent loading models — the
prize is enormous — and **an oracle recovers none of it.** The 5.9% figure is
partial overlap: it assumes a preload can start, be interrupted by a real
request, and resume, which is not how a load behaves, and which the placement
manager correctly refuses to attempt anyway.

The argument that does not depend on any estimate: the largest idle lead
anywhere in the campaign is **1,256 ms**, and the shortest load actually
measured is **1,601 ms**.

**Two independent measurements agree.** The placement manager, running live,
skipped every one of its 24-26 cycles per run because the node was busy. The
trace replay, done afterwards and by different means, finds the same absence of
idle time. Neither was derived from the other.

## Why prediction cannot fix this

Prediction changes *when you decide*, not *whether there is a window to act in*.
A perfect predictor and a random one both need the engine to be free long enough
to load a model. On a workload that keeps the engine busy, the better predictor
buys nothing, and the failure is structural rather than a matter of accuracy.

That is why this is a decision and not another deferral. Building it would
produce a component that is measurably correct and measurably never runs — which
is what Phase 3 already produced, and doing it twice would be the mistake.

## The reopening condition was tested, and it fired

Rather than leave the condition above as a caveat, it was measured. `loadgen.py`
gained `--think-ms`, a pause between rounds imitating a human turn, and Phase 3
was re-run with 8 s gaps against a 3.7 s load.

**Reactive placement started working.** Cold starts 9 → 8 per run, wall-clock
−6.3% at 25/25 pairwise, p95 flat. One preload per run, and the cold starts it
removed are the repeat-after-eviction kind — the only kind a preload can remove.
`docs/PHASE3-RESULTS-PRESSURE.md` has the campaign.

**That strengthens this decision rather than weakening it.** The oracle ceiling
on the same traces is **5 of 45** cold starts hideable, up from 0 in the closed
loop. Reactive placement removed **5**. It captured the entire ceiling, watching
demand and predicting nothing.

So on the workload where predictive placement finally has room to act, a
reactive manager already occupies all of it. Prediction would be competing for
zero remaining headroom. The earlier argument was "there is no window"; the
better one is "there is a window, it is small, and something simpler already
fills it".

## What would reopen it

Not a better model. A workload with idle capacity, which means one of:

- **Spare nodes.** Preloading onto a node that is idle because nothing needs it
  costs nothing and takes as long as it likes. Both scenarios here keep every
  node busy. This is the most likely place for predictive placement to pay, and
  it is a cluster-shape question, not a prediction question.
- ~~**Bursty traffic with real gaps.**~~ Tested — see above. It opens a window,
  reactive placement fills all of it, and predictive placement is left with
  nothing to add. This condition is now closed rather than open.

If either is measured and shows idle time exceeding load time, `predictive_ceiling.py`
will say so on that trace, and this decision should be revisited. The tool
exists precisely so that reopening it is a measurement rather than an argument.

## What this does not claim

- **Not that transition prediction is useless.** It works, on the workload where
  there are enough models for it to mean anything. It is the acting, not the
  predicting, that has nowhere to go.
- **Not that placement is wrong.** It ships implemented and off, with its
  skip-when-busy rule intact — that rule is the reason placement did not cause
  the p95 regression §9 forbids.
- **Not a general result.** Three simulated campaigns now, one cluster shape.
  The closed-loop objection has been answered by measurement rather than
  argument; the remaining one is spare capacity, which is a cluster-shape
  question this project has not been able to pose with two nodes that are both
  busy.
- **The ceiling is small enough to be scenario-specific.** Five hideable cold
  starts out of 45 comes from gaps that fall only between rounds. A workload
  with a gap after *every* request would have a larger ceiling, and whether
  reactive placement would still capture all of it is untested.
