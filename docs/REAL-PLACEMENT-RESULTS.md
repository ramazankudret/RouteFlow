# Placement on real hardware: D34's condition does not transfer

D34 concluded that reactive placement is conditional — inert on back-to-back
traffic, worth having once the workload leaves gaps to preload into. Both halves
came from simulated nodes. Run against two real engines, **the first half holds
and the second does not.**

Setup is `bench/real_two_node.sh`'s: a GPU engine and a CPU engine, both under
`OLLAMA_MAX_LOADED_MODELS=1`, two small models alternating. Five runs per arm,
16 requests per run, same policy and cost model in both arms — only placement
differs. The adapter's load and drop were verified against this Ollama first.

## Back-to-back: confirmed

| | LRU | placement |
| --- | ---: | ---: |
| wall-clock median (s) | 45.25 | 45.06 |
| — pairwise | | 15/25, inside variance |
| cold starts per run | **8** | **8** |
| preloads per run | — | **0-2** |
| cycles skipped, node busy | — | 23-40 |

The manager is starved exactly as in simulation: it skips nearly every cycle for
want of an idle node, cold starts are identical, and wall-clock is
indistinguishable. **This part of D34 transfers.**

## With 8 s gaps: it acts, and it still does not pay

| | LRU | placement |
| --- | ---: | ---: |
| wall-clock median (s) | 90.49 | 93.93 |
| — pairwise | | 10/25, inside variance |
| cold starts per run | 8 | **7** |
| **preloads per run** | — | **6-7** |
| **evictions the manager counted** | — | **0** |
| ttft p50 (ms) | 792 | **1,560** |

In simulation the same gaps produced a 6.3% win at 25/25 pairwise. Here the
manager gets its window, uses it six or seven times a run, removes exactly one
cold start, and loses on every latency line.

## Why: a preload is a swap, and the manager cannot see it

Where the cold starts went:

| | LRU | placement |
| --- | --- | --- |
| gpu / `qwen2.5:0.5b` | 19 of 19 cold | **12 of 23 cold** |
| gpu / `tinyllama` | 19 of 40 cold | **25 of 40 cold** |

Placement did not remove cold starts. It **moved** them: it warmed one model on
the GPU and cooled the other by the same amount, netting one.

The manager's own counters give it away — **6 to 7 preloads and 0 evictions**.
It believes it is adding residency for free. It is not: `OLLAMA_MAX_LOADED_MODELS=1`
means the engine holds one model, so loading `qwen` throws `tinyllama` out. The
engine performs that eviction implicitly and the manager never records it.

The root cause is a gap in the node-state contract, not in the manager's
reasoning. `NodeState` carries `engine_slots` — *parallel requests the engine
accepts* — and nothing at all about **how many models the engine will keep
resident**. Placement therefore decides on `vram_free_bytes` alone, and on this
cluster the GPU reports ~7 GB free against two 400 MB models: the router
believes both fit comfortably. Ollama disagrees, and never says so.

**A simulated node cannot contain this defect**, because its residency capacity
*is* its VRAM by construction. `bench/profiles/pressure-*.json` gave each node
room for two models, so a preload there genuinely added one. That is why the
simulated campaign said placement pays and the hardware says it does not.

## What this changes

- **D34's second half is withdrawn for engines with a resident-model limit.**
  Not disproved in general: on a node that can genuinely hold two models a
  preload still adds rather than swaps, which is what the simulation measured.
  It is the transfer that fails, and the condition D34 named — "traffic leaves
  gaps" — turns out to be necessary but not sufficient.
- **The missing condition is engine capacity in models, not bytes.** Until the
  node state carries it, the placement manager cannot tell "preload B" from
  "swap A for B", and those have opposite value.
- The manager's `evictions` counter reads 0 through all of this, which is
  accurate about what *it* did and misleading about what happened. A counter
  that only sees its own actions is not measuring the system.

## What this does not show

- **Two engines on one machine.** A preload competes with the request stream for
  CPU and PCIe here in a way two separate machines would not, so some of the
  TTFT regression is the substitute setup rather than placement itself.
- **`MAX_LOADED_MODELS=1` is a deliberate choice**, made so that eviction would
  be real with two small models. It is also exactly the setting that turns a
  preload into a swap, so this result speaks to that configuration, which is a
  common one on a single card.
- Both arms sit inside run-to-run variance on wall-clock. The finding is the
  mechanism — the swap and the uncounted eviction — not the sign of a 3.8%
  difference measured at 10/25.

## Reproduce

```bash
bench/real_placement.sh --runs 5 --rounds 8 --think-ms 0
bench/real_placement.sh --runs 5 --rounds 8 --think-ms 8000
```
