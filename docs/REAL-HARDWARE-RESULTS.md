# The learned cost model, measured on a real GPU

Every comparative result in this repository was measured against simulated
nodes. This one was not.

**RTX 4060 Laptop, 8188 MiB. Ollama in Docker with GPU passthrough,
`qwen2.5:7b-instruct-q4_K_M`. One node, 90 requests.**

## Why this run had to happen

D37 found that on the OpenAI streaming path a real engine reports no token
counts unless asked, and the router was not asking. `observe()` learns the
decode rate from `output_tokens` and the prefill rate from
`prompt_tokens_actual`; with both null it learns nothing, so **the learned cost
model behaved exactly like the static one against any real engine.** The
simulated nodes always sent the counts, which is why five campaigns never
noticed.

That was fixed. This run asks whether the fix took.

## Result

| | static-v1 | learned-v1 | |
| --- | ---: | ---: | --- |
| **median timing error** | 39.6% | **17.9%** | **−54.7%** |
| paired wins | | **26/30** | sign test p = 5.9e-05 |
| worst three errors | 66, 65, 59% | 59, 55, 53% | |
| records carrying `output_tokens` | 30/30 | 30/30 | |
| records carrying `prompt_tokens_actual` | 30/30 | 30/30 | |

The counts arrive on every record now, and the learned model roughly halves the
prediction error on real hardware.

**The comparison is paired.** Both arms are driven from the same seed, so
request *n* in one arm has the same prompt and the same cap as request *n* in
the other. That is a stronger test than comparing two distributions, and it is
what the sign test is computed over.

The protocol is D25's, unchanged: the learned arm replays a warm-up trace
written by an earlier static run and never learns from the run being measured
on itself.

## What it learned: the seed table is optimistic about this card

One matched request, term by term:

```
            predicted  =  prefill  +  decode      actual
static           1507  =       59  +    1448        2182
learned          2230  =      400  +    1829        2449
```

The seeded prefill rate is out by about seven times. It comes from
`cost.gpu_seeds`, keyed on the GPU name, and it was written from a spec sheet
rather than from this machine — which is exactly what the learned model exists
to replace, and exactly the error a simulated node cannot contain, because a
simulated node's rates are whatever the profile says they are.

Both terms move upward. The card is slower than the table assumed, and the model
found that out by reading thirty of its own traces.

## What this does not show

- **One run per arm, thirty requests each.** The pairing is strong and the sign
  test is not marginal, but this is a single sitting. The simulated campaigns
  report five runs precisely because one is not enough to see run-to-run spread,
  and that discipline is not met here.
- **The arms ran in sequence, learned last.** Pairing controls for the prompt
  and the cap; it does not control for the machine drifting between arms. A
  proper version interleaves them, as `bench/phase2.sh` does.
- **One node, so no routing happened.** Every decision is `single_candidate`.
  This measures the estimator, not the scheduler: wall-clock, cold starts and
  every policy comparison are untouched by it and remain simulated-only. A real
  test of the *routing* claim needs a second GPU.
- **`load_ms` is still null.** Only Ollama's native path reports
  `load_duration`, and the router proxies the shape the client sent (§2, D18).
  Load bandwidth is therefore still derived from ttft rather than measured, as
  D19 describes.
- **One model, one quantisation, one card.** The 7× prefill error is this
  machine's, not a general claim about the seed table.

## Reproduce

```bash
bench/real_learning.sh --requests 30
```

Needs a reachable Ollama with the model pulled. The script writes its traces to
`bench/results-real-learning/` and prints the comparison.
