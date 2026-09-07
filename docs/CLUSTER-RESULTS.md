# On a real network, and when a node leaves it

Every result in this repository until now was measured over `127.0.0.1`, where a
connection either succeeds immediately or is refused immediately. The router has
a whole layer for the cases in between — `NodeStale` admission, `poll_failures`,
retry to another node, and §6.4's rule that once a byte has reached the client
there is no retry — and **none of it had ever been executed against a node that
was genuinely unreachable.**

`bench/real_cluster.sh` puts each node in its own container on a Docker bridge:
the engine bound to that container's loopback, the agent in front of it as the
only reachable port, authenticating. That is D18's topology, running for the
first time as designed. The router is a container on the same network, so the
router–agent hop is real TCP across a real bridge, resolved by name.

This is not two machines. It is one machine's CPU, RAM and card. What it does
give is a real network namespace per node, a real name resolution, real
cgroup limits, and — the point — real ways to take a node away.

## What the faults found

`bench/real_cluster.sh faults` runs five, and asserts what should happen rather
than that nothing crashed. The fault always lands on **whichever node the
baseline actually used**, read out of the trace: the first version faulted a
fixed node and passed everything while that node had served zero of the 21
baseline requests.

| fault | how | what the router did |
| --- | --- | --- |
| partition | `docker network disconnect` | `resolve rf-node-b: No address associated with hostname`, node dropped, work moved |
| freeze | `docker pause` | `no response` — a timeout, a different path from a refusal |
| slow link | `tc netem delay 300ms` | still a node; 300 ms each way does not trip a 5 s staleness horizon |
| death under load | `docker kill` mid-campaign | no request lost |
| engine dies, agent lives | `pkill ollama` inside the container | retried on another node |

Three distinct diagnostics for three distinct failures is worth more than it
sounds. "Node unreachable" with no reason is the least useful line a scheduler
can log, and this one says which of the three happened.

All fourteen assertions pass. **One of them did not, and that is the result.**

## D42: the retry went back to the node that had just failed

The fifth fault is the only one that reaches §6.4's retry branch on purpose:
the engine dies while its agent stays up, so the node still looks healthy, the
router dispatches, the proxy to the engine fails, and nothing has reached the
client yet — exactly the condition under which a retry elsewhere is allowed.

It returned **502, three times out of three**, with a healthy node sitting
admitted beside it. The trace showed both attempts on the same node:

```
node=a outcome=dispatch_failed retry_of=no   cand a: admitted reason=ok
node=a outcome=dispatch_failed retry_of=yes  cand a: admitted reason=ok
                                             cand b: admitted reason=ok
```

The dispatcher assembled the exclusion list correctly. `reserve_locked` built it
correctly. It then **never passed it to the policy**, and applied it afterwards
by walking the finished candidate list and marking excluded nodes rejected —
with `if (node == winner) continue;`, because marking the winner rejected would
have left a Decision with no usable winner.

So the exclusion did nothing **precisely when an excluded node was the best
one**, which is the only case that matters. That defeated D9's retry, and it
equally defeated the operator's own `excluded` flag (§6.1): `--exclude` a node
and the router would keep using it as long as it kept winning.

The fix is to give `IPolicy::select` the list so admission rejects those nodes
with a reason, and delete the patch-up. Admission already had the machinery —
`admit()` takes an exclusion list and returns `AdmitReason::Excluded`. Nobody
had ever handed it one.

**Why five simulated campaigns never saw it.** On loopback the engine and the
agent die together, so a node that fails is a node that has already gone
unhealthy — it loses on admission before the exclusion list is ever consulted.
The bug needs a node that is *up and unusable*, and that requires the engine and
the agent to be separable, which requires them to be separate processes behind a
network. The container is what made it reachable.

**What covers it now.** A unit test pins the policy contract: an excluded node
that would have won does not win, appears as rejected with its reason, and
excluding everything yields no winner rather than a winner nobody may use. That
test does **not** cover the wiring — reverting the fix leaves all 108 checks
green, because the test calls `select` directly and the defect was that nobody
called it with the list. The regression test for the wiring is fault 5, and
reverting the fix turns it red: `RETRIES total=3 moved=0 same_node=3`, three
502s. Some bugs live between two correct components and only an integration
test can see them.

## The second cluster shape

Every real result so far came from one shape: a GPU that decodes eight to twenty
times faster than the alternative. Both open caveats depend on that ratio being
large — `docs/TTFT-DECISION.md` declines a TTFT-aware objective *because* the
warm node is the slow one, and says so. One point is not a curve.

**The CPU pair does not work, and that is worth recording.** Two throttled CPU
engines at 14 cores against 1 give 32.8 against 25.3 tok/s — a ratio of 1.3. A
0.5B model on llama.cpp is bound by memory bandwidth, not cores, so core count
is not a heterogeneity knob at all. It is the sort of thing that is obvious to
assume and takes one measurement to disprove.

**Two engines on the same card do work**, for this question: 305.2 against 303.8
tok/s, a ratio of 1.008. They share 8 GB and one SM array, so they contend in a
way two machines would not — but the question is what happens when the warm node
is *not slower*, and this is that.

One more thing had to change. With two nodes and two models a warmth-aware
router simply gives each model a node, nothing is ever evicted again, and there
is no warmth question left: measured, 1 cold start in 20 requests. **The
interesting regime is models outnumbering nodes**, so the campaign drives three.
The harness now refuses to report a run with fewer than three cold starts, since
"the objectives agree" would otherwise be a fact about the workload.

## Reproduce

```bash
bench/real_cluster.sh up          # build the cluster on a bridge
bench/real_cluster.sh faults      # five faults, fourteen assertions
bench/real_cluster.sh ratio gpu   # two comparable nodes, and measure the ratio
bench/real_cluster.sh ratio-campaign 10
bench/real_cluster.sh down
```
