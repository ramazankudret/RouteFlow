# The uncertainty band: what it claimed, and what it delivered

Every estimate this router makes carries a 1-sigma band, and D8 uses it for a
real purpose: when the gap between the best two candidates is smaller than
either band, the decision is labelled `within_noise` rather than presented as a
considered choice. That label appears in the trace, in the UI, and in every
campaign result this project has published.

So the band has to mean something. A 1-sigma band should contain the outcome
about **68%** of the time. This is what it actually did.

## The measurement

`bench/sigma_calib.py` replays recorded traces in order. Each rule sees a
record's prediction, produces a band from what earlier records taught it, and is
*then* shown the outcome. Nothing is ever scored against data it has already
absorbed, so the coverage below is out of sample.

| corpus | coverage, shipped band | median sigma |
| --- | ---: | ---: |
| real 2-node (warmth) | **14%** | 230 ms |
| real placement, 8 s gaps | **7%** | 280 ms |
| simulated Phase 3, 8 s gaps | 69% | 409 ms |
| simulated Phase 2, capped | 81% | 567 ms |
| simulated Phase 2, uncapped | 48% | 342 ms |

The simulated average of 69% looks healthy and is not: it is `load: 22%` and
`warm: 93%` averaged together. The band was too wide where the router was
confident and far too narrow where it was not, which is the one arrangement
that is worse than a constant.

## Why

Sigma was assembled from exactly two things:

```
sigma = sqrt( (output_sigma / decode_rate)^2  +  (t_load * f)^2 )
```

- the **output length** uncertainty, converted to milliseconds, and
- a **guessed fraction** `f` of the load time -- 0.12 once load bandwidth was
  learned, 0.30 before.

Which means the band described *how many tokens the reply might be* and almost
nothing else. It contained:

- **no queue term.** Measured coverage in the queued regime: 5-10%.
- **no prefill term.**
- **nothing for a rate being wrong.** The decode rate is divided into the
  length as though it were exact. On real hardware, records whose output length
  was predicted *to the token* still missed by a median of 92 ms.
- **nothing for being wrong about the world.** The largest real errors come
  from the router believing a model is resident when the engine has quietly
  swapped it out (D38). The band had no way to express "my picture of this node
  may be stale", which is precisely the uncertainty that mattered most.

`f` was never measured. It is the last guessed constant in a cost model whose
whole premise is that constants get replaced by measurements.

## The fix (D39)

The analytic band stays, and gets a **floor**: the residual this cost model's
own predictions have actually shown on this node in this regime.

```
sigma = max( analytic_band, learned_MAD(node, regime) * 1.2533 )
```

- **Regime** is `load`, `queue`, or `warm`, taken from the terms the estimate
  itself priced. The error distributions are genuinely different and pooling
  them reproduces the original defect in a new place.
- **1.2533** converts a mean absolute deviation to a standard deviation for a
  normal distribution. It is taken from the distribution, not fitted to these
  traces, so the coverage claimed below is a prediction that could have failed.
- **A floor, never a term.** Adding it in quadrature would let a wide history
  swamp a genuinely confident estimate. A floor only ever refuses to claim
  precision the node has never delivered -- so every rate Phase 2 learned still
  sharpens the band, and a node that really is predictable keeps its sharpness.
- **Learned only from this model's own records**, gated on `cost_model`. A
  replayed trace was written by whichever model was running at the time, and
  adopting its misses would floor our band with somebody else's error. That is
  D31's trap, in a new place; the test checks it explicitly.

### What the rules scored

Five rules were compared on all five corpora before any C++ was written. The
one that ships (R5) was not the widest or the highest-coverage; it was the only
one that improved every corpus **without widening a band that was already
honest**.

| rule | real 2-node | real placement | sim P3 | sim P2 uncapped | sim P2 capped |
| --- | ---: | ---: | ---: | ---: | ---: |
| shipping (analytic) | 14% | 7% | 69% | 48% | 81% |
| learned residual, replaces analytic | 52% | 69% | 83% | 72% | 67% |
| relative residual, pooled by regime | 60% | 77% | 85% | 73% | 66% |
| **analytic + learned floor (R5)** | **52%** | **69%** | **86%** | **81%** | **89%** |

The pooled rule scores higher on the real corpora and is rejected anyway: its
advantage is that one shared key converges faster, not that it models anything.
It would hand a CPU node the GPU's reliability.

Note the capped Phase 2 row: replacing the analytic band with a learned
residual makes that corpus **worse** (81% to 67%), because there the analytic
band was doing real work. That is the whole argument for a floor rather than a
replacement, and it is the reason the rules were scored on five corpora instead
of the one that motivated the fix.

## What the shipped code did

The table above scores the *rule*, offline. This is the binary, re-running two
whole campaigns.

**Real two-node cluster** (`bench/real_two_node.sh`, 5 runs × 16 requests):

| | before | after |
| --- | ---: | ---: |
| coverage, all records | **14%** | **65%** |
| — warm, unqueued | 25% | 59% |
| — cold | 3% | 70% |
| — queued | 0% | 67% |
| median sigma | 230 ms | 2,981 ms |
| median \|error\| | 1,659 ms | 1,947 ms |
| decisions labelled `within_noise` | 13/80 (16%) | **46/80 (57%)** |

That last row is the correction, and it is not a small one. The router was
presenting 84% of its choices as considered decisions. Fewer than half of them
were distinguishable from noise.

**Simulated Phase 2** (`bench/phase2.sh`), run to check the fix costs nothing
where the band was already working:

| | before | after |
| --- | ---: | ---: |
| coverage, learned-v1 | 80% | 84% |
| `within_noise` | 59/125 | **59/125** |
| wall-clock median | 77.4 s / 76.1 s | unchanged within variance |
| cold starts per run | 3 | 3 |

Not one `within_noise` label moved, and no routing changed. On that corpus the
analytic band was already wider than the floor almost everywhere, so the floor
almost never binds — which is what a floor is supposed to do.

## What this does not fix

- **It is a band, not a mean.** A floor cannot correct a prediction that is
  systematically 2.8 s low on cold starts; it can only stop the router
  claiming that prediction is precise. The mean error on real hardware is
  D38's missing resident-model limit, and it stays open.
- **A floor can only widen, which is a cost as well as a safety property.**
  On the capped Phase 2 corpus the analytic band was already covering 100% of
  warm records — too wide, not too narrow — and a floor cannot fix that
  direction. Coverage there is 84%, above the honest 68%. The fix addresses
  bands that claim too much, not bands that claim too little.
- **The warm regime is still lumpy on real hardware.** One EWMA per node and
  regime has to cover both clean warm serves and the ones where the engine
  silently swapped the model out (D38), and those are two distributions. The
  result is a band roughly 3× the median warm error — honest on average, too
  wide for the easy half. Splitting that key is only worth doing once D38 is
  closed, because right now the two cases are genuinely indistinguishable to
  the router.
- **Published `within_noise` counts change.** Every campaign result in this
  repository was computed with a band 7-12x too narrow in the cold regime, so
  its `within_noise` figures understate how many decisions were coin flips.
  The wall-clock and cold-start numbers are unaffected -- sigma has never
  influenced routing, only its description (`router/policy/candidates.cpp`
  picks the minimum either way).

## Reproduce

```bash
# score candidate rules out of sample on any corpus
python3 bench/sigma_calib.py "real 2-node" 'bench/results-real-two-node/warmth-v1-run*.jsonl'

# decompose the residual by term, to see which one the band is missing
python3 bench/sigma_decomp.py "real 2-node" 'bench/results-real-two-node/warmth-v1-run*.jsonl'
```

`sigma_decomp.py` compares load and prefill as one term wherever the engine did
not report a load time, rather than pretending the split is known. Its first
version charged an unmeasured load to prefill and invented a 1.5 s prefill bias
that was not there — the same class of mistake the band itself was making.
