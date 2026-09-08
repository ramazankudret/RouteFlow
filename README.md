# RouteFlow

A warmth-aware inference scheduler for a small, heterogeneous cluster of local
GPUs. Several machines run their own inference engine; RouteFlow decides which
one serves each request, and it decides by pricing the whole cost in
milliseconds — including the one everything else ignores:

```
T_total = T_queue + T_load + T_prefill + T_decode + T_evict
```

`T_load` is what it costs to bring a model into VRAM. A queue-depth router
cannot see it, so it will happily send a request to an idle fast node that must
spend seven seconds loading, while a busy slower node already holds the model
warm. Deciding that trade-off in one unit, with an uncertainty on it, is the
whole project.

C++17, no third-party libraries. JSON, HTTP/1.1 and SSE are in `common/`.

## Is this for you

It helps if all of these are true:

- **More than one machine with an inference engine**, and they are *different* —
  different card, or a card and a CPU. Two identical nodes have no trade to make;
  one node has nothing to route.
- **More models than nodes.** With a model per node a warmth-aware router simply
  gives each one a home and the question disappears — measured, 1 cold start in
  20 requests. The problem exists when something must always lose its place.
- **Batch or agent traffic**, where what matters is when the whole job finishes.
  If a person is watching the first token appear, read `docs/TTFT-DECISION.md`
  first: RouteFlow makes that number *worse*, deliberately, and measuring the
  alternative did not produce a better answer.

It will do nothing for a single GPU, and it is not a serving framework — it
routes to the engines you already run, speaking their APIs.

## Build

Needs CMake ≥ 3.16 and a C++17 compiler. Developed on WSL Ubuntu with g++ 13.

```bash
cmake -S . -B build
cmake --build build -j4
```

Produces `routeflow-agent` and `routeflow-router`.

## Test

```bash
cd build && ctest --output-on-failure
```

Three suites. `selftest` covers `common/` — JSON, HTTP, SSE, the trace round
trip. `policytest` covers admission, scoring, the ledger and the cost models.
`snapshot_auth` drives the built binaries over a real socket, because what it
checks — that `/snapshot` refuses a request without the shared bearer token, and
that the control surface stays closed — is a property of the running server and
not of any linkable function.

## Run

### Against a real engine

One agent per machine, beside the engine it observes. The engine stays on
loopback; only the agent is exposed, and the agent authenticates (D14, D18).

```bash
routeflow-agent --node.id desktop --engine.endpoint 127.0.0.1:11434 \
                --http.bind 0.0.0.0 --http.token SECRET
```

Then the router, with a `nodes.json` naming each agent:

```json
{ "nodes": [ { "id": "desktop", "endpoint": "192.168.1.12:8971" } ] }
```

```bash
routeflow-router --nodes nodes.json --trace trace.jsonl \
                 --policy warmth-v1 --cost_model learned-v1 \
                 --node.token SECRET --http.token CLIENT_SECRET
```

It speaks the OpenAI and Ollama chat APIs, so existing tooling points at it
unchanged. The operator console is at `http://127.0.0.1:8970`.

One caveat worth knowing before you point a client at it. On the OpenAI shape a
streamed reply carries no token counts unless they are asked for, and the cost
model cannot learn without them, so the router adds
`stream_options: {include_usage: true}` to streaming OpenAI requests. Your
client therefore receives one extra final chunk carrying `usage` and an empty
`choices` array — standard OpenAI, but it will break a client that assumes
`choices[0]` is always present. Turn it off with `--dispatch.request_usage
false` and the router is a byte-for-byte proxy again, with the learned cost
model reduced to its seeds on that path (D37).

### Against simulated nodes

A heterogeneous cluster on one machine, which is how every published result in
`docs/` was produced — reproducibly, and without uncontrolled thermals:

```bash
routeflow-agent --simulate bench/profiles/sim-desktop.json --http.port 8981 --node.id sim-desktop
routeflow-agent --simulate bench/profiles/sim-jetson.json  --http.port 8982 --node.id sim-jetson
routeflow-agent --simulate bench/profiles/sim-laptop.json  --http.port 8983 --node.id sim-laptop
```

## Measure

Each phase has a harness and a documented exit criterion, and each reports the
median of five runs with the pairwise comparison, not a single run.

```bash
bench/compare.sh                      # Phase 1: warmth against round-robin
bench/phase2.sh                       # Phase 2: learned cost model against static
bench/phase2.sh --uncapped            #   ... on a workload that states no max_tokens
bench/phase3.sh                       # Phase 3: placement against the engine's LRU
bench/phase3.sh --think-ms 8000       #   ... with gaps between turns
bench/real_learning.sh                # the estimator against a real engine
bench/real_two_node.sh                # Warmth vs RoundRobin on a GPU and a CPU engine
bench/real_placement.sh               # placement on real engines, at both think times
```

And on a real network, with each node in its own container and its own network
namespace — the topology the agent exists for, and the only place the router's
failure handling can actually be exercised:

```bash
bench/real_cluster.sh up              # nodes on a Docker bridge, engine on loopback
bench/real_cluster.sh faults          # partition, freeze, 300ms netem, kill, engine death
bench/real_cluster.sh concurrency     # concurrent sub-agents against real engines
bench/real_cluster.sh ratio gpu       # a second cluster shape: two comparable nodes
bench/real_cluster.sh down
```

Analysis over any trace, no re-run needed:

```bash
python3 bench/trace_report.py trace.jsonl
python3 bench/summarize.py bench/results-phase2 --baseline static-v1 --policy learned-v1
python3 bench/prior_counterfactual.py "bench/results-*/static-v1-run*.jsonl"
python3 bench/predictive_ceiling.py "bench/results-*/lru-run*.jsonl"
```

## Documents

| | |
| --- | --- |
| `docs/ARCHITECTURE.md` | the design, and the phase plan with its exit criteria |
| `docs/DECISIONS.md` | every decision, why, and which ones a measurement overturned |
| `docs/TRACE-SCHEMA.md` | the trace contract — versioned, append-only |
| `docs/PHASE1-RESULTS.md` | warmth beats round-robin, and how much of it is warmth |
| `docs/PHASE2-RESULTS.md` | the learned cost model |
| `docs/PHASE2-UNCAPPED-RESULTS.md` | the same, without a caller-supplied `max_tokens` |
| `docs/PHASE3-RESULTS.md` | **retracted** — it measured the wrong scenario, and says so |
| `docs/PHASE3-RESULTS-PRESSURE.md` | the corrected campaign |
| `docs/PHASE4-DECISION.md` | predictive placement: measured, and declined |
| `docs/REAL-HARDWARE-RESULTS.md` | the estimator on an actual GPU |
| `docs/REAL-TWO-NODE-RESULTS.md` | Warmth against RoundRobin on two real engines, GPU and CPU |
| `docs/REAL-PLACEMENT-RESULTS.md` | placement on real engines: a preload turned out to be a swap |
| `docs/UNCERTAINTY.md` | what the 1-sigma band actually covered, and the floor it got |
| `docs/TTFT-DECISION.md` | why the TTFT regression is not fixed, measured against the alternative |
| `docs/CLUSTER-RESULTS.md` | the cluster on a real network, and what happens when a node leaves it |
| `docs/UI-BRIEF.md` | the operator console's brief (Turkish) |

## What is not claimed

Most comparisons in `docs/` were measured on **simulated nodes**, whose rates
come from `bench/profiles/` — seeded from real hardware but not equal to it.
What transfers from those is the mechanism and the sign of each effect, not the
seconds.

Two are not simulated:

- `docs/REAL-HARDWARE-RESULTS.md` — the learned cost model against the static
  one on an actual RTX 4060. Median timing error roughly halves, 26 of 30 paired
  requests improve, and the seeded prefill rate for that card turns out to be
  about seven times optimistic.
- `docs/REAL-TWO-NODE-RESULTS.md` — Warmth against RoundRobin, on two real
  engines: one on the card, one on the CPU. Warmth is about a quarter faster on
  wall-clock, 25/25 pairwise, **while taking seven times more cold starts** —
  which is the premise working rather than failing. That page also keeps the
  pre-D38 figures and says how far they moved, and reports that the baseline
  itself swings 16% between campaigns.
- `docs/TTFT-DECISION.md` — the loss that every one of those pages reported.
  Warmth's first token is 40% later. Measured against the alternative, a
  TTFT-aware objective does not move the median first token *at all* and makes
  the complete reply arrive nearly three times later at p95. Declined, the way
  Phase 4 was.

These are single sittings on one machine, weaker evidence than the five-run
simulated campaigns, and each document says where it is weak. TTFT in particular
goes the wrong way in the two-node run, and that is reported rather than buried.

- `docs/CLUSTER-RESULTS.md` — the same cluster with each node in its own
  container and its own network namespace, so a node can be partitioned, frozen,
  slowed or killed. Fourteen assertions across five faults. It is where D42 was
  found, and it is the reason the failure handling is no longer merely written.

**What is still not tested: two physical GPUs.** Every heterogeneous result here
comes from a card paired with a CPU engine, or from two engines sharing one
card. The mechanism does not depend on that, but the seconds do, and so does one
conclusion — `docs/TTFT-DECISION.md` measures both ratios precisely because its
answer turns on which shape you have.

The reports say what did not work as plainly as what did. Phase 3 failed its
exit criterion on saturated traffic and met it once the workload left gaps, and
on real hardware it does not pay at all. Phase 4 was closed by measurement
rather than built. One published result was retracted outright, and one headline
figure was cut in half by a later fix — both are still on the page, next to what
replaced them.

Real hardware found seven defects that five simulated campaigns could not: a 503
after a load, learning that was inert against a real engine, a preload that was
really a swap, an uncertainty band covering 7-14% of outcomes where it claimed
68%, every guard in the benchmark harness reporting success while it printed its
refusal, a benchmark that threw away what its own agents had learned ten times
over, and — once the nodes were put behind a real network — a retry that went
back to the node that had just failed, because an exclusion list was built and
never handed over. `docs/DECISIONS.md` D36 through D42.

That is the honest summary of the project's state: the four phases are closed
with measured exit criteria, the premise is demonstrated on real engines, on a
real network and under concurrent load, and **nobody has run it but its
author**. Every failure mode
listed above is one a benchmark happened to walk into. The next one will be
found by whoever points it at traffic that was not designed to test it.
