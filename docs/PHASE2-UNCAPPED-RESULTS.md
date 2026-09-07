# Phase 2, re-measured without `max_tokens`

Phase 2 passed, and its report named the hole it passed through:

> **Output-length learning was never exercised.** Every one of the 125 records
> carried a caller-supplied `max_tokens` [...] A workload without `max_tokens`
> is the obvious next measurement.

This is that measurement. It turned out to be worth more than the original.

## Numbers

Same scenario, same seed, same policy (Warmth), same cluster, same warm-up
protocol. The only change is that the load generator no longer states
`max_tokens`, so the node draws the reply length itself and the router has to
predict it (D29).

| | static-v1 | learned-v1 | | capped campaign |
| --- | ---: | ---: | --- | ---: |
| **wall-clock median (s)** | 81.29 | **74.15** | −8.8% | −1.9% |
| — pairwise | | **25/25** | p < 0.05 | 24/25 |
| **cold starts per run** | 16 | **3** | −81% | 3 vs 3 |
| **length error (median)** | 265.7% | **16.2%** | −94% | 0% vs 0% |
| timing error (median) | 176.0% | **17.2%** | −90% | 8.6% → 1.7% |
| ttft p50 (ms) | 2,137 | 583 | −73% | |
| ttft p95 (ms) | 8,591 | 3,236 | −62% | |
| total p50 (ms) | 3,006 | 2,179 | −28% | |
| total p95 (ms) | 13,031 | 13,241 | **+1.6%** | |
| within 1σ | 34% | 48% | (~68% is honest) | 74% / 81% |

Five runs each. The capped column is the committed Phase 2 campaign, for scale:
the same code, the same cluster, and a five-fold larger effect once the caller
stops answering the question the scheduler is supposed to answer.

The `0%` length error in the capped column is not a good score. It is the
measurement reporting that nothing was measured: predicted equalled the cap
equalled actual, in both arms.

**The one line that goes the other way is total p95**, 1.6% worse. It is a
single order statistic over one tail and no pairwise test supports it; it is
recorded here because it is the only number that moved against the result.

## What actually happened: the prior was outranking warmth

The interesting failure is not that 256 tokens is a poor guess. It is what a
poor guess does to the ranking.

`T_decode` is the largest term. Assuming 256 tokens for replies that are 40-90
inflates it roughly four-fold — on **every candidate**, so it does not cancel.
It grows until it swamps `T_load`, and at that point a warmth-aware router has
stopped being warmth-aware. It is not ignoring warmth by design; warmth has
simply been priced out of the comparison by a term that is wrong.

The trace shows it directly. One worker request, static arm, whose reply turned
out to be 88 tokens:

```
                total    queue    load  prefill  decode   evict
sim-desktop     9,584        0   2,484      211   2,772   4,118   <- chosen, COLD
sim-laptop     16,951    8,476       0      599   7,877       0   <- already warm
```

`sim-desktop` was chosen while paying a load *and* an eviction, because the
assumed 256 tokens made `sim-laptop`'s 7,877 ms decode term the largest number
on the board. At the true 88 tokens those decode terms are 953 and 2,708 ms,
and — since a static model's queue term is built from its own prefill and
decode estimate, so it shrinks too — `sim-laptop` wins at roughly 6,600 against
7,800. The warm node was there the whole time; it was priced out.

Replaying every decision with `t_decode` rescaled to the length the reply
actually had (`bench/prior_counterfactual.py`, the same trace-replay technique
Phase 1 used):

```
routing counterfactual over 125 decision(s) with a choice:
  scaling t_decode to the true length moves 30/125 of them (24%)
  30 of those move from a cold node to a warm one
```

**All thirty.** Not a mixed bag with a net gain — every single decision the
prior changed was a decision to go cold instead of warm. And 30 is a floor:
the replay rescales `t_decode` only, while the queue term on a static model
carries a decode estimate of its own that would shrink with it.

That is what put 75 of the static arm's 125 jobs on `sim-desktop`, a node that
cannot hold both models — and **all 75 of them were cold starts.**

| | sim-desktop | sim-jetson | sim-laptop | cold total |
| --- | ---: | ---: | ---: | ---: |
| static-v1 | 75 (75 cold) | 25 (5 cold) | 25 | **80/125** |
| learned-v1 | 50 (10 cold) | 50 (5 cold) | 25 | **15/125** |

## "You only won because 256 was the wrong constant"

The obvious objection, and it is answerable from the trace without another run.
`t_decode` is linear in the token count, so substituting a different prior is
exact arithmetic on the recorded breakdown:

```
timing error as run            176.0 %
tuned single prior (  70 tok)   34.2 %   <- one constant, chosen after the fact
tuned per-model prior           34.2 %   <- the ceiling for any fixed prior
oracle (actual length)          36.2 %   <- knowing each reply exactly
```

So a hindsight-tuned constant does recover most of the accuracy — 176% to 34% —
and the learned model is still twice better at 17.2%, because it also replaced
the seeded decode, prefill and load rates. Length was the largest single error,
not the only one.

Two things in that table are worth more than the headline:

- **A tuned prior needs a number nobody has in advance.** 70 tokens is the
  median of the replies this workload happened to produce. Setting it requires
  having already run the thing you are trying to schedule.
- **The oracle is *worse* than the tuned constant** — 36.2% against 34.2%.
  Perfect length knowledge scores below a hand-picked wrong one, which means
  the tuned constant is not being accurate, it is cancelling a different bias
  in the static rates. That is exactly what hand-tuning produces and exactly
  why it does not survive: fix the other error and the tuned constant becomes
  wrong again, silently.

## Two defects the measurement found

Neither was visible in the code, and neither could have been exposed by the
capped campaign, where predicted equals actual by construction.

**The trace recorded the caller's cap, not the model's prediction (D30).**
`predicted_output_tokens` was written from `RequestFeatures`. Capped, the two
agree exactly and nothing looks wrong. Uncapped it was **zero** — and zero is
indistinguishable from "no prediction", so the length error was unmeasurable
and `observe()` skipped its error EWMA entirely. The learning loop was open in
precisely the case it exists for: it could learn the mean, and could never
learn how wrong the mean was.

**Sigma was the error of whichever model wrote the trace (D31).** The residual
was measured against the record's own prediction. The learned arm is
deliberately fed a trace written by the static arm (D25), so it opened every
run believing its own error was ~190 tokens while the mean it had learned was
within ~3. Sigma started at 192 tokens, 122 of 125 decisions came back
`within_noise`, and 97% of jobs landed inside a band claiming to be 1σ.

`within_noise` is a label rather than a fallback, so routing was unharmed —
the corrected campaign moved the wall-clock from 74.42 s to 74.15 s, inside
variance. The damage was to what the system says about itself: a console
reporting that 98% of its choices are indistinguishable from noise, on a
workload it fits to 16%, is describing a scheduler nobody should trust.
The residual is now measured against the model's own prior, and `within_noise`
falls to 59/125 — matching the capped campaign exactly.

> **Superseded in part by D39.** Every `within_noise` count on this page was
> computed with a band that has since been measured and found 7-12× too narrow
> in the cold regime — so the real number of coin-flip decisions is *higher*
> than reported here, not lower. Wall-clock, cold starts and every other figure
> are unaffected: sigma has never influenced routing, only its description.
> See `docs/UNCERTAINTY.md`.

## What this does not show

- **The two error figures come from different runs.** static-v1's 176% is
  measured on a run that took 16 cold starts; learned-v1's 17.2% on one that
  took 3. Cold starts are the hardest jobs to predict, so part of that gap is
  the routing, not the estimator. The counterfactual section is the part that
  isolates the prior.
- **Sigma is now slightly too narrow**, not right: 48% coverage against the 68%
  a 1σ band claims. Better than the 34% it replaced and better than a band that
  covered 97%, but the estimator is now mildly overconfident and that is
  untuned on purpose — the next thing to fix, not something to trim into shape.
- **Simulated nodes.** The magnitudes belong to `bench/profiles/`, not to
  hardware. What transfers is the mechanism: an output-length prior large
  enough to dominate `T_decode` will outrank warmth on any cluster.
- **`footprint_bytes` is still seeded** (D24). Unchanged by any of this.
- **The learned arm still gets a warm-up trace it did not write** (D25). That is
  the protocol, not an accident, and D31 is what happens when the model forgets
  the trace came from somewhere else.

## The capped campaign still holds — and is noisier than it looked

D30 and D31 touch the cost model, so the capped result was re-run on the new
code: three runs per arm, same scenario, same protocol.

| | static-v1 | learned-v1 | committed campaign |
| --- | ---: | ---: | --- |
| timing error (median) | 11.8% | **6.0%** | 8.6% → 1.7% |
| length error | 0.0% | 0.0% | 0.0% → 0.0% |
| cold starts per run | 3 | 2 | 3 → 3 |
| within_noise | | 31/75 | 59/125 |

Direction and structure hold: the learned model still halves the timing error,
length error is still exactly zero in both arms (the cap, again, answering the
question for everyone), and `within_noise` sits at the same fraction. Wall-clock
at n=3 is 7/9 pairwise, which is inconclusive and expected to be.

But the absolute levels moved on both arms at once, and chasing that turned up
something about the **original** Phase 2 number.

**The five runs of the learned arm are not five samples of the learned model.**
They share one warm-up trace, because `run_once` writes `warmup.jsonl` once and
every learned run replays it (D25). So the published 1.4-2.3% spread is the
spread of one draw re-measured, not the spread of the method. Replaying a
different warm-up on the same binary gives 5.2%; the run that first flagged
this gave 13.7%.

Checked rather than assumed: holding the warm-up fixed and re-running on the
new binary gives a **signed** bias of +0.3% on warm jobs, so nothing systematic
was introduced — the level is drifting, not the model. A code defect shows up
as bias; this does not.

What protects every comparison here is that `phase2.sh` interleaves the arms:
static run 1, learned run 1, static run 2, and so on. When the machine drifts,
both arms drift together. The *ratio* between arms is the result; the absolute
error figure belongs to the campaign that produced it and should be quoted that
way — including the 1.7% in the Phase 2 report.

## Reproduce

```bash
bench/phase2.sh --uncapped --out bench/results-phase2-uncapped --runs 5 --rounds 5 --warmup 10
python3 bench/summarize.py bench/results-phase2-uncapped --baseline static-v1 --policy learned-v1
python3 bench/prior_counterfactual.py "bench/results-phase2-uncapped/static-v1-run*.jsonl"
```

The capped campaign still runs unchanged, and still reproduces its committed
numbers.
