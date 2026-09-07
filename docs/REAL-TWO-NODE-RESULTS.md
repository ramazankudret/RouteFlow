# Phase 1's question, answered on real hardware

Warmth against RoundRobin has only ever been measured against simulated nodes,
because a heterogeneous cluster appears to need more than one GPU. It does not.

**Two Ollama engines on one machine: one with the RTX 4060, one on the CPU.**
Separate processes, separate memory, separate resident sets — genuinely
independent, and genuinely different. Measured before anything was built:

```
GPU   load 2.5-5.4 s   decode 230-274 tok/s
CPU   load 1.0-1.7 s   decode  12- 28 tok/s
```

The asymmetry is the interesting part, and it is the one this project was
designed around: **the GPU decodes an order of magnitude faster and loads two to
three times slower**, because it must move weights across PCIe while the CPU
engine only maps them. So the crossover is real:

```
GPU cold:  4000 + 4N ms        CPU warm:  50N ms        equal at N ~ 87 tokens
```

Output lengths in the workload run 32 to 192 tokens, straddling that line on
purpose. Neither node is the right answer for every request.

## Result

Re-measured on the current router. D38 changed how it prices a load on an engine
that keeps one model, so the first version of this page described a router that
could not see the swap it was paying for; those numbers are kept at the bottom.

| | roundrobin-v1 | warmth-v1 | |
| --- | ---: | ---: | --- |
| **wall-clock median (s)** | 48.81 | **42.60** | **−12.7%** |
| — spread | 46.2-50.5 | 40.8-47.0 | |
| — pairwise | | **24/25** | p < 0.05 |
| total p95 (ms) | 10,129 | **4,251** | −58.0% |
| total p50 (ms) | 2,587 | 2,690 | +4.0% worse |
| ttft p50 (ms) | 905 | 1,980 | **+119% worse** |
| ttft p95 (ms) | 1,626 | 3,808 | **+134% worse** |
| cold starts per run | **1** | **12** | |

Five runs each, 16 requests per run, identical traffic from one seed.

## Warmth wins by taking *more* cold starts, not fewer

The expected story is that a warmth-aware policy avoids loads. That is not what
happened, and what did happen is a better demonstration of the premise.

| | gpu | cpu | cold starts |
| --- | ---: | ---: | ---: |
| roundrobin-v1 | 40, all `qwen2.5:0.5b` | 40, all `tinyllama` | **4** |
| warmth-v1 | 72 — 38 qwen **and 34 tinyllama** | 8 | **59** |

RoundRobin fell into **perfect model affinity by accident**. Two nodes, two
models, both alternating in lockstep: each node saw only ever one model, so
after the first load nothing was ever evicted. Its baseline here is the best one
it could possibly have had.

Warmth put **nine tenths** of the traffic on the GPU, including the model the
GPU had to keep reloading. It paid **fifteen times more cold starts and won by
12.7% anyway** — because a 2.2 s load buys decoding that is eight to twenty
times faster, and pricing both in milliseconds is the only way to see that.

That is the whole claim of `T_total = T_queue + T_load + T_prefill + T_decode +
T_evict`, and this is the first time it has been shown outside simulation.

### The load term is paid, not decisive — and the earlier page had that wrong

The first version of this page cited the attribution line as agreement:
*"dropping `t_load` would change 60 of 80 of Warmth's decisions."* On the
current router it changes **2 of 80**, and the honest reading is not that the
term stopped mattering.

Before D38 the router believed a node was warm when the engine had already
swapped the model out, which manufactured a `t_load` difference between two
candidates that were both, in fact, cold. Removing the term removed that
phantom difference and flipped decisions. It was measuring a bug.

Now both candidates usually do have to load, so `t_load` is close to common
between them and dropping it rarely changes the ranking. What decides is
`t_decode` — 40 of 80 decisions, against 40 inside the noise band. Which is
exactly the crossover this page opens with: **the load is not free, it is
affordable.** Warmth chooses the GPU knowing it will pay about 2.2 s (the
median `t_load` on its winning candidate), because
the reply is worth more than that. The term is doing its job by being *in* the
sum, not by dominating it.

## What went the other way

- **TTFT is worse, and materially.** p95 goes from 1,626 ms to 3,808 ms, and
  p50 more than doubles.
  Warmth's loads land in front of the first token, so a user watching a cursor
  waits longer while the batch as a whole finishes sooner. §9 makes wall-clock
  the criterion for agent workloads, where nobody is watching intermediate
  replies, but for an interactive workload this trade is the wrong way round.
- **The magnitudes are still worse than the ranking.** Timing error is 34% for
  Warmth against 26% for RoundRobin. The band itself is now honest — 84% of jobs
  land inside a 1-sigma band, against 14% when this page was first written,
  because D39 gave sigma a measured floor. The policy is making good decisions
  with imprecise estimates, and now says so.
- Warmth's error being the worse of the two is not a paradox: it routes to nodes
  that must load, and load is the term with the widest uncertainty.

## What this does not show

- **One machine.** Both engines share a CPU, RAM and PCIe bus, so the CPU node's
  work slows the GPU node's host side. A real two-machine cluster does not have
  that coupling, and it is the reason this is a substitute for a second GPU
  rather than a replacement.
- **The baseline was lucky.** RoundRobin's zero evictions come from the node
  count and model count both being two and both alternating. Three models, or a
  random order, and it would thrash. That makes this workload *favourable* to
  RoundRobin, which strengthens the result rather than weakening it, but it
  should not be read as RoundRobin's typical behaviour.
- **Two small models, one seed, one pair of engines.** The crossover at ~87
  tokens is this pair's, not a general number.
- **Both arms use `learned-v1`.** The seed table is keyed on a GPU name and the
  CPU node reports none, so a seeded model cannot tell these two nodes apart —
  that would have handed Warmth a handicap unrelated to warmth.

## Reproduce

```bash
docker run -d --name rf-gpu --gpus all -p 11436:11434 \
  -e OLLAMA_MAX_LOADED_MODELS=1 -v rf_models:/root/.ollama ollama/ollama:latest
docker run -d --name rf-cpu -p 11435:11434 \
  -e OLLAMA_MAX_LOADED_MODELS=1 -e CUDA_VISIBLE_DEVICES="" \
  -v rf_models:/root/.ollama ollama/ollama:latest
docker exec rf-gpu ollama pull qwen2.5:0.5b
docker exec rf-gpu ollama pull tinyllama:latest

bench/real_two_node.sh --runs 5 --rounds 8
```

`OLLAMA_MAX_LOADED_MODELS=1` is what makes the eviction real: without it both
models sit in memory and there is no warmth to be aware of.

## The first measurement, kept

Before D38 the router could not see that loading a model on this engine evicts
the resident one, so it priced some loads at zero. Same harness, same workload:

| | roundrobin-v1 | warmth-v1 | |
| --- | ---: | ---: | --- |
| wall-clock median (s) | 62.20 | 47.16 | −24.2%, 25/25 |
| ttft p95 (ms) | 1,980 | 4,477 | +126% worse |
| cold starts per run | 0 | 8 | |
| within 1 sigma | — | 14% | |

The conclusion survived the correction and the margin did not: −24.2% became
−12.7%, still outside run-to-run variance. What changed qualitatively is the
attribution, above. Kept here because a result that moves when a bug is fixed
should show how far it moved.
