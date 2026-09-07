# Phase 2 result

**Verdict: the exit criterion is met, on both halves.** §9 asks for measurably
lower prediction error *and* a wall-clock improvement over Phase 1's Warmth, and
either alone would not have counted.

| | static-v1 | learned-v1 | |
| --- | ---: | ---: | --- |
| **timing error (median)** | 8.6% | **1.7%** | −80% |
| **wall-clock (median, s)** | 77.62 | **76.13** | −1.9% |
| ttft p50 (ms) | 1,274 | 581 | −54% |
| ttft p95 (ms) | 3,323 | 3,324 | — |
| total p50 (ms) | 2,278 | 2,187 | −4.0% |
| total p95 (ms) | 14,532 | 14,545 | — |
| cold starts per run | 3 | 3 | — |

Five runs each, same scenario, same seed, same policy (Warmth), same cluster.
The only difference is which cost model scores the candidates.

## The wall-clock win is small but not noise

Per-run, sorted:

```
learned-v1   75.70  75.83  76.13  76.32  77.23
static-v1    77.08  77.37  77.62  77.77  77.92
```

The min-max ranges touch — 77.23 against 77.08 — and reading only those would
say "overlapping, inconclusive". They are the wrong summary for two samples of
five. Every other comparison goes one way: **24 of 25 pairings favour the
learned model**, which for n=5,5 clears the exact Mann-Whitney threshold of 23
at p < 0.05. `bench/summarize.py` now prints the pairing count for exactly this
reason, because the range summary misled the first reading of this result.

That said, 1.9% is a small effect and the honest framing is that Phase 2 bought
**accuracy first and speed second**. Cold starts did not change at all (3 per
run in both arms): the learned model did not route around more loads, it
predicted the same routing more precisely.

## Where the 80% error reduction came from — and where it did not

The learned model replaced the seeded decode rate, prefill rate and load
bandwidth with measured ones. That is the whole of it.

**Output-length learning was never exercised.** Every one of the 125 records
carried a caller-supplied `max_tokens`, because the load generator sets it so a
simulated node performs a deterministic amount of work. A caller's cap is a real
upper bound on its own reply and beats anything the model inferred, so
`predict_output` returned the cap in both arms and the learned length EWMA sat
unused.

This matters for reading the result two ways:

- The 80% reduction is **understated** relative to a real agent workload, where
  most callers do not cap and the largest term's input would come from the
  learned length rather than a blind 256-token prior.
- It also means the noise band barely moved: `within_noise` fell only from
  64/125 in Phase 1 to 59/125 here. Sigma is dominated by
  `predicted_output_sigma`, which in this benchmark comes from the ingest
  heuristic's 35%-of-cap rule, not from measured error. The thing that would
  narrow it is precisely the thing the benchmark bypasses.

> **Superseded in part by D39.** Every `within_noise` count on this page was
> computed with a band that has since been measured and found 7-12× too narrow
> in the cold regime — so the real number of coin-flip decisions is *higher*
> than reported here, not lower. Wall-clock, cold starts and every other figure
> are unaffected: sigma has never influenced routing, only its description.
> See `docs/UNCERTAINTY.md`.

A workload without `max_tokens` is the obvious next measurement, and it is a
scenario change rather than a code change.

**It has since been run: `docs/PHASE2-UNCAPPED-RESULTS.md`.** The effect is five
times larger there (-8.8% wall-clock, 25/25 pairwise; cold starts 16 -> 3), and
the reason is not accuracy for its own sake -- an output-length prior big enough
to dominate `T_decode` outranks `T_load`, and a warmth-aware router stops being
warmth-aware. The measurement also found two defects that a capped workload
cannot expose, because a cap makes predicted, actual and cap the same number
(D30, D31). The numbers in the table above are unaffected and still reproduce.

## The 1.7% is this campaign's number, not a constant

Every learned run above replays the same `warmup.jsonl` — one draw, re-measured
five times (D25). The 1.4-2.3% spread across runs is therefore the stability of
that draw, not of the method: a different warm-up on the same code gives 5.2%,
and a three-run re-check on a busier machine gives 6.0% against a static arm
that also rose, to 11.8%.

The comparison survives because `phase2.sh` interleaves the two arms, so both
drift together and the ratio is what is being measured. The absolute figure
should be quoted with its campaign attached. See
`docs/PHASE2-UNCAPPED-RESULTS.md`.

## What is still seeded

`footprint_bytes` was seeded here (D24) and no longer is: the engine reports
resident VRAM per model and the router now learns the overhead from it (D35).
The paragraph below is what was believed at the time, and it was wrong about
the data — the trace has no post-load reading, but the live node state always
did.
Closing it needs a second VRAM reading after a load settles — an agent and
schema change that belongs to whichever phase needs the accuracy, not to this
one. It is left visible rather than quietly approximated, because a footprint
error is an admission failure and shows up as a routing bug, not a slow reply.

## Method

The learned arm is given a 10-round warm-up trace written by an earlier static
campaign and replayed at startup through `--replay_from`. It never learns from
the run being measured: an arm that improved *during* its own measurement would
be reporting its warm-up, not its model. `--replay_from` is deliberately
separate from `--trace` for that reason (D25).

Reproduce with:

```bash
bench/phase2.sh --runs 5 --rounds 5 --warmup 10
python3 bench/summarize.py bench/results-phase2 --baseline static-v1 --policy learned-v1
```

## What this does not show

- **Simulated nodes.** The magnitudes belong to `bench/profiles/`, not to
  hardware. What transfers is that splitting the prediction and learning each
  part separately beats a seeded table, and the sign of the effect.
- **One scenario, one seed, capped outputs.** See above — the capped outputs in
  particular understate what the learned model does on real agent traffic.
- **A cold learned model is the static model.** With no history it matches its
  seed exactly, which `tests/policy_test.cpp` asserts. The gain here is what ten
  rounds of history buys; a fresh cluster gets nothing until it has run.

## Phase 2 exit checklist

| §9 requirement | state |
| --- | --- |
| `LearnedCostModel` implementing `ICostModel`, fed by `observe()` | done |
| Cold-start seeding from the static table | done — matches the seed exactly with no data |
| `alpha` learned from contended jobs | done, and asserted not to corrupt the base rate |
| Prediction-error panel in the UI | done — `ui/accuracy.html`, timing and length errors on separate plots |
| **Exit criterion:** lower prediction error **and** a wall-clock improvement | **met** — −80% error, −1.9% wall-clock at p < 0.05 |
