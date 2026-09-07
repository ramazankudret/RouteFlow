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

## Closing it: what the missing number was worth

`NodeState` now carries `models_resident_limit`, the agent infers it, and the
router acts on it in three places (D38). Re-running this campaign at each stage
gives three points, and the middle one is the interesting one.

| | lru cold starts / run | gpu / cpu requests | `t_load` priced |
| --- | ---: | ---: | ---: |
| before | 8 | 59 / 21 | 39 of 80 |
| the limit, and belief corrected | **14** | 75 / 5 | 70 of 80 |
| … and the eviction dated | **3** | 45 / 35 | 14 of 80 |

**The half-fix made it worse.** Telling the router that a load displaces
something, without telling it what the displacement costs, sent nearly all the
traffic to the GPU: correct about every request in isolation, blind to what each
one cost the next. Cold starts went from 8 a run to 14.

The missing half was that **T_evict has never fired on real hardware.** Ollama
reports when a model expires, never when it was used, so `last_used_ms` arrived
as 0 from every real engine — the LRU victim order was arbitrary and
`evicts_warm` was permanently false. The externality D7 exists to price was live
in simulation and dead in the field, on every real decision this project has
measured. The router knows the answer, because it knows what it dispatched, so
admission now dates the victim from the ledger.

With both halves, the router **specialises the two engines on its own**: qwen
settles on the CPU (30 of 35 of its requests), tinyllama on the GPU (35 of 45),
and cold starts fall to 3 a run against the original 8. Nobody told it to
partition the models. It is what "loading here costs the resident model its
place" implies, once that sentence is expressible.

Note where that improvement lands: in the **LRU arm**. It is better routing, not
better placement.

## Placement itself still does not pay here

| | lru | placement |
| --- | ---: | ---: |
| wall-clock median (s) | 95.72 | 95.63 |
| — pairwise | | 14/25, a coin flip |
| cold starts per run | **3** | **5** |
| ttft p50 (ms) | 688 | 1,384 |
| preloads / evictions per run | — | 1 / 1-2 |

The manager no longer swaps blindly — preloads fell from six or seven a run to
one, and the evictions it does make are now counted rather than performed
invisibly by the engine. But it is now working against the router rather than
with it: the router has settled tinyllama on the GPU, and the manager keeps
preloading qwen there because its demand counter still says qwen is wanted on
that node eight times in the window. Every one of those preloads costs tinyllama
its place, and `gpu/tinyllama` goes from 5 of 35 cold in the LRU arm to **10 of
10 cold** with placement on.

That is D34's question, not D38's, and it is left open: the manager's demand
signal counts requests *routed* to a node, which is a measure of where the router
has been sending work rather than of where the work would be best served. The
two disagree exactly when the router has just learned something.

## Reproduce

```bash
bench/real_placement.sh --runs 5 --rounds 8 --think-ms 0
bench/real_placement.sh --runs 5 --rounds 8 --think-ms 8000
```

The "before" rows above predate D38 and are kept as measured. So do the figures
in `docs/PHASE1-RESULTS.md` and the real two-node campaign: D38 changes routing
on any engine with a resident-model limit, so anything measured against real
Ollama before it describes a router that could not see the swap it was paying
for.
