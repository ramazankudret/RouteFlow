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

## What would reopen it

Not a better model. A workload with idle capacity, which means one of:

- **Spare nodes.** Preloading onto a node that is idle because nothing needs it
  costs nothing and takes as long as it likes. Both scenarios here keep every
  node busy. This is the most likely place for predictive placement to pay, and
  it is a cluster-shape question, not a prediction question.
- **Bursty traffic with real gaps.** An interactive agent session is quiet
  between a human's turns — tens of seconds, against a 3.7 s load. That is a
  window, and it is the realistic case this project has never benchmarked,
  because both scenarios are closed loops that issue the next request as soon as
  the last one returns.

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
- **Not a general result.** Two simulated scenarios, one cluster shape, closed-
  loop load generation. The closed loop is the specific thing that removes idle
  time, and it is an artefact of the benchmark, not of local inference. That is
  stated as the strongest argument *against* this decision, and it is why the
  reopening conditions above are written down rather than left implied.
