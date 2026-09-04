# Simulated cluster profiles

Three nodes for the Phase 1 comparison (D16). They are not a toy: the exit
criterion needs a heterogeneous *and reproducible* cluster, and a policy
comparison run against live hardware with uncontrolled thermals and background
load is not a measurement.

## The cluster

| node | VRAM | decode (planner / worker) | load bandwidth | holds both models? |
| --- | --- | --- | --- | --- |
| `sim-desktop` | 8 GB | 30 / 90 tok/s | 1.0 GB/s | **no** — 7 + 2 > 8 |
| `sim-jetson` | 15 GB | 14 / 42 tok/s | 1.2 GB/s | yes |
| `sim-laptop` | 6 GB | — / 34 tok/s | 0.4 GB/s | worker only |

Two models, sized like a real agent workload: a `planner:12b` at 7 GB and a
`worker:3b` at 2 GB.

## Where the numbers come from

They are chosen to be *physically defensible*, not to produce a particular
result. Getting this wrong in the flattering direction would make the Phase 1
verdict worthless.

- **Decode is memory-bound.** An RTX-class 8 GB part has roughly 2.5-3× the
  memory bandwidth of an Orin, and the decode rates follow that ratio rather
  than being picked independently. The desktop's 30 tok/s on a 12B q4 and the
  Jetson's 14 tok/s are both in the range those parts actually produce.
- **Prefill is compute-bound** and runs 30-40× the decode rate on each node.
- **Load bandwidth is a storage path, not a memory path**, so it is roughly
  1 GB/s everywhere and does not track the decode ordering. The Jetson is
  slightly *faster* here despite being slower at everything else, because
  unified memory means a model load is not crossing PCIe. That inversion is
  the point: it is exactly the kind of thing queue-depth routing cannot see.
- **`sim-laptop` cannot fit the planner at all** (7 GB into 6 GB), so it is a
  permanent `insufficient_vram` rejection for planner traffic and a real
  candidate for worker traffic. A cluster where every node is admissible does
  not exercise admission.

## The pressure that makes this interesting

`sim-desktop` cannot hold both models: 7 + 2 = 9 GB into 8 GB. Alternating
planner and worker traffic therefore evicts one to load the other, every time.
`sim-jetson` holds both and stays warm.

So the cluster poses the question the project is about: is it better to send a
planner call to the fast node that must spend 7 seconds loading, or to the slow
node that already has it? The answer is genuinely not obvious — it depends on
how many tokens come out, which is why `T_decode` and `T_load` have to be
compared in the same units instead of one being a "warmness bonus".

For a 200-token planner reply: the desktop pays 7000 ms of load plus 6667 ms of
decode; the Jetson pays 0 plus 14286 ms. The desktop wins that one by a nose.
Shorten the reply to 120 tokens and the Jetson wins. Neither policy should be
expected to win everywhere — what Phase 1 measures is which wins across a
realistic *mix*, over total scenario wall-clock (D12).

## Running

```bash
routeflow-agent --simulate bench/profiles/sim-desktop.json --http.port 8981 --node.id sim-desktop
routeflow-agent --simulate bench/profiles/sim-jetson.json  --http.port 8982 --node.id sim-jetson
routeflow-agent --simulate bench/profiles/sim-laptop.json  --http.port 8983 --node.id sim-laptop
```

`seed` is per node and fixes its jitter stream, so a scenario replays
identically. Change it only to check that a result is not an artefact of one
particular jitter draw.
