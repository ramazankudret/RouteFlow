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

| | roundrobin-v1 | warmth-v1 | |
| --- | ---: | ---: | --- |
| **wall-clock median (s)** | 62.20 | **47.16** | **−24.2%** |
| — spread | 60.1-69.5 | 44.8-52.0 | |
| — pairwise | | **25/25** | p < 0.05 |
| total p95 (ms) | 12,450 | **5,688** | −54.3% |
| total p50 (ms) | 3,174 | 2,934 | −7.6% |
| ttft p50 (ms) | 1,362 | 1,622 | **+19% worse** |
| ttft p95 (ms) | 1,980 | 4,477 | **+126% worse** |
| cold starts per run | **0** | **8** | |

Five runs each, 16 requests per run, identical traffic from one seed.

## Warmth wins by taking *more* cold starts, not fewer

The expected story is that a warmth-aware policy avoids loads. That is not what
happened, and what did happen is a better demonstration of the premise.

| | gpu | cpu | cold starts |
| --- | ---: | ---: | ---: |
| roundrobin-v1 | 40, all `qwen2.5:0.5b` | 40, all `tinyllama` | **2** |
| warmth-v1 | 60 — 40 qwen **and 20 tinyllama** | 20 | **38** |

RoundRobin fell into **perfect model affinity by accident**. Two nodes, two
models, both alternating in lockstep: each node saw only ever one model, so
after the first load nothing was ever evicted. Its baseline here is the best one
it could possibly have had.

Warmth put three quarters of the traffic on the GPU, including the model the
GPU had to keep reloading. It paid **nineteen times more cold starts and won by
24% anyway** — because a 2.5 s load buys decoding that is eight to twenty times
faster, and pricing both in milliseconds is the only way to see that.

That is the whole claim of `T_total = T_queue + T_load + T_prefill + T_decode +
T_evict`, and this is the first time it has been shown outside simulation.

The attribution line agrees: dropping `t_load` from the score would change 60 of
80 of Warmth's decisions. The term is doing the work.

## What went the other way

- **TTFT is worse, and materially.** p95 goes from 1,980 ms to 4,477 ms.
  Warmth's loads land in front of the first token, so a user watching a cursor
  waits longer while the batch as a whole finishes sooner. §9 makes wall-clock
  the criterion for agent workloads, where nobody is watching intermediate
  replies, but for an interactive workload this trade is the wrong way round.
- **The estimator is badly calibrated on this cluster.** Timing error is 48% for
  Warmth against 27% for RoundRobin, and only 14% of jobs land inside a band
  claiming to be 1-sigma, against the ~68% that would be honest. The policy is
  making good decisions with poor estimates — the ranking is right more often
  than the magnitudes are.
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
