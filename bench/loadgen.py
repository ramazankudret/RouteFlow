#!/usr/bin/env python3
"""RouteFlow load generator — the Phase 1 scenario (ARCHITECTURE §9).

Replays a fixed multi-agent workload: each round is one planner call on a large
model followed by several concurrent sub-agent calls on a small one. That shape
is the point. A single-model workload has no warmth to be aware of, and a
workload with no concurrency never exercises the queue or contention terms.

Reproducibility is a requirement, not a nicety (D12): prompt sizes and output
lengths come from a seeded RNG, so two runs with the same seed issue
byte-identical requests. By default `max_tokens` is set explicitly, which makes
a simulated node perform exactly the modelled amount of work — but it also
hands the router the one number it is supposed to predict, so `--uncapped`
withholds it and lets the node draw the length from the same range instead.

  python3 bench/loadgen.py --router http://127.0.0.1:8970 --rounds 6
  python3 bench/loadgen.py --policy warmth-v1 --rounds 6 --seed 42
  python3 bench/loadgen.py --uncapped --rounds 5     # output length unknown

Wall-clock is printed here because it is the metric Phase 1 is judged on; every
other number comes from the trace file via bench/trace_report.py.
"""

import argparse
import concurrent.futures
import json
import random
import sys
import time
import urllib.error
import urllib.request

# The simulated cluster's two models. Overridable because this generator is the
# only concurrent one in bench/, and until now it had only ever been pointed at
# simulated nodes -- so T_queue, the contention alpha and the herd protection
# (§6.2, §6.3, D4, D5) had never met a real engine, which has its own internal
# queue and its own parallelism rather than a modelled one.
PLANNER_MODEL = "planner:12b"
WORKER_MODEL = "worker:3b"

# The pressure scenario (Phase 3). Four models on nodes that hold two, accessed
# with a skew: one model is asked for most of the time and three others rotate
# through. That pattern is what defeats LRU — the rotating models sweep the hot
# one out of VRAM even though it is the one about to be needed again — and it is
# the ordinary situation on a local cluster with more models than memory.
#
# It is not a pattern chosen to flatter placement. Placement wins here only if
# frequency is a better eviction signal than recency; if it is not, this
# scenario says so just as clearly.
PRESSURE_HOT = "hot:4b"
PRESSURE_COLD = ["cold-a:4b", "cold-b:4b", "cold-c:4b", "cold-d:4b", "cold-e:4b"]
PRESSURE_HOT_SHARE = 0.5


def make_prompt(rng, approx_tokens):
    """Filler sized to a token count. Content is irrelevant to a simulated node
    (it charges for prefill by token count) but the size must be realistic,
    because prompt length drives T_prefill on real hardware too."""
    words = ["context", "state", "plan", "step", "tool", "result", "note",
             "input", "output", "check", "value", "field", "case", "path"]
    # ~0.75 words per token for English prose.
    count = max(1, int(approx_tokens * 0.75))
    return " ".join(rng.choice(words) for _ in range(count))


def post(router, path, payload, headers, timeout):
    data = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(router + path, data=data, method="POST")
    request.add_header("Content-Type", "application/json")
    for key, value in headers.items():
        request.add_header(key, value)
    started = time.monotonic()
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read()
            return {
                "ok": True,
                "ms": (time.monotonic() - started) * 1000.0,
                "node": response.headers.get("X-RouteFlow-Node", "?"),
                "warm": response.headers.get("X-RouteFlow-Warm") == "1",
                "bytes": len(body),
            }
    except urllib.error.HTTPError as exc:
        return {"ok": False, "ms": (time.monotonic() - started) * 1000.0,
                "error": f"HTTP {exc.code}: {exc.read()[:200].decode('utf-8', 'replace')}"}
    except Exception as exc:  # noqa: BLE001 - a failed request is data, not a crash
        return {"ok": False, "ms": (time.monotonic() - started) * 1000.0,
                "error": str(exc)}


def call(router, model, role, prompt_tokens, output_tokens, token, timeout, rng_stream,
         stream=True, capped=True):
    # Streaming by default, because that is what agent tooling does and because
    # time-to-first-token only exists for a stream: on a buffered reply the
    # first byte is the whole answer, so the router records ttft as null rather
    # than as a number describing something else. A non-streamed bench run
    # reports no TTFT at all, which §9 asks Phase 1 to publish.
    payload = {
        "model": model,
        "messages": [{"role": "user", "content": make_prompt(rng_stream, prompt_tokens)}],
        "stream": stream,
    }
    # `output_tokens` is drawn either way, so the RNG stream — and therefore
    # every prompt size — is identical between capped and uncapped runs. Only
    # whether the caller *states* the number changes. Uncapped, the node draws a
    # length from the same range itself and the router has to predict it.
    if capped:
        payload["max_tokens"] = output_tokens
    headers = {"X-RouteFlow-Role": role}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    result = post(router, "/v1/chat/completions", payload, headers, timeout)
    result["role"] = role
    result["model"] = model
    return result


def set_policy(router, policy, token):
    payload = json.dumps({"policy": policy}).encode("utf-8")
    request = urllib.request.Request(router + "/admin/policy", data=payload,
                                     method="POST")
    request.add_header("Content-Type", "application/json")
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.loads(response.read())


def active_policy(router, token):
    """What the router is actually running. Recorded rather than assumed: a
    result labelled with the policy we *meant* to set would lie silently if the
    switch had failed."""
    request = urllib.request.Request(router + "/admin/stats")
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            return json.loads(response.read()).get("policy")
    except Exception:  # noqa: BLE001
        return None


def think(args, rng, round_index):
    """The gap a human leaves between turns.

    Both benchmarks are closed loops: the next request goes out the moment the
    last one returns, so the cluster is never idle and a placement manager never
    gets a window to preload into. That is a property of the harness, not of
    local inference -- an interactive agent session is quiet while somebody
    reads the answer and types the next thing.

    PHASE4-DECISION.md names this as the condition that would reopen predictive
    placement, so it has to be measurable rather than argued about.

    The jitter comes from a *separate* stream. Drawing it from the same RNG that
    picks models would advance that stream and change which models the run asks
    for, so a think-time run and a closed-loop run would differ in two ways at
    once and neither could be attributed. Same seed, same request sequence, at
    every think value -- which is what D12 is for.
    """
    if args.think_ms <= 0:
        return 0.0
    seconds = args.think_ms / 1000.0 * (0.5 + rng.random())
    time.sleep(seconds)
    return seconds


def run_pressure(args):
    """Skewed access over a working set larger than any node's VRAM."""
    rng = random.Random(args.seed)
    # Its own stream, so pauses cannot move the model sequence (see think()).
    think_rng = random.Random(args.seed ^ 0x7417)
    results = []
    started = time.monotonic()
    idle = 0.0
    cold_cycle = 0

    for round_index in range(args.rounds):
        if round_index:
            idle += think(args, think_rng, round_index)

        batch = []
        for _ in range(args.subagents + 1):
            if rng.random() < PRESSURE_HOT_SHARE:
                batch.append(PRESSURE_HOT)
            else:
                batch.append(PRESSURE_COLD[cold_cycle % len(PRESSURE_COLD)])
                cold_cycle += 1

        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            # Deliberately serial: the point is the *order* models are touched
            # in, and concurrency would blur which eviction followed which
            # request.
            futures = [pool.submit(call, args.router, model, "subagent",
                                   rng.randint(300, 600), rng.randint(40, 90),
                                   args.token, args.timeout,
                                   random.Random(args.seed + round_index * 100 + i),
                                   args.stream, args.capped)
                       for i, model in enumerate(batch)]
            for future in futures:
                result = future.result()
                results.append(result)
                if not result["ok"]:
                    print(f"  round {round_index + 1}: {result['error']}",
                          file=sys.stderr)

        print(f"  round {round_index + 1}/{args.rounds} done "
              f"({time.monotonic() - started:.1f}s elapsed)", file=sys.stderr)

    # Wall-clock now contains time nobody was waiting on the cluster, so it is
    # returned separately rather than quietly folded into the primary metric.
    return results, time.monotonic() - started - idle


def run_scenario(args):
    if args.scenario == "pressure":
        return run_pressure(args)

    rng = random.Random(args.seed)
    results = []
    started = time.monotonic()

    for round_index in range(args.rounds):
        # The planner thinks first; the sub-agents fan out from its output. That
        # dependency is what makes the workload alternate models, which is what
        # puts a node with too little VRAM under eviction pressure.
        planner = call(args.router, args.planner_model, "planner",
                       rng.randint(900, 1500), rng.randint(110, 170),
                       args.token, args.timeout, rng, args.stream, args.capped)
        results.append(planner)
        if not planner["ok"]:
            print(f"  round {round_index + 1}: planner failed: {planner['error']}",
                  file=sys.stderr)

        jobs = [(rng.randint(300, 600), rng.randint(40, 90))
                for _ in range(args.subagents)]
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.subagents) as pool:
            futures = [pool.submit(call, args.router, args.worker_model, "subagent",
                                   prompt, output, args.token, args.timeout,
                                   random.Random(args.seed + round_index * 100 + i),
                                   args.stream, args.capped)
                       for i, (prompt, output) in enumerate(jobs)]
            for future in concurrent.futures.as_completed(futures):
                result = future.result()
                results.append(result)
                if not result["ok"]:
                    print(f"  round {round_index + 1}: subagent failed: "
                          f"{result['error']}", file=sys.stderr)

        print(f"  round {round_index + 1}/{args.rounds} done "
              f"({time.monotonic() - started:.1f}s elapsed)", file=sys.stderr)

    return results, time.monotonic() - started


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--router", default="http://127.0.0.1:8970")
    parser.add_argument("--policy", help="switch the router to this policy first")
    parser.add_argument("--rounds", type=int, default=6)
    parser.add_argument("--subagents", type=int, default=4,
                        help="concurrent sub-agent calls per round")
    parser.add_argument("--seed", type=int, default=7,
                        help="fixes prompt and output sizes; same seed, same requests")
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--scenario", choices=("agent", "pressure"), default="agent",
                        help="agent: planner plus concurrent sub-agents (Phases 1-2). "
                             "pressure: skewed access over a working set larger "
                             "than VRAM (Phase 3)")
    parser.add_argument("--no-stream", dest="stream", action="store_false",
                        help="send buffered requests; TTFT is then unmeasurable")
    parser.add_argument("--think-ms", type=int, default=0,
                        help="mean pause between rounds, imitating a human turn. "
                             "Zero (the default) is a closed loop: the next "
                             "request goes out the moment the last returns, and "
                             "the cluster is never idle")
    parser.add_argument("--uncapped", dest="capped", action="store_false",
                        help="omit max_tokens, as most agent callers do. The node "
                             "then draws the reply length itself and the router "
                             "has to predict it — which is the only way the "
                             "learned output-length model is exercised at all")
    parser.set_defaults(stream=True, capped=True)
    parser.add_argument("--token", help="bearer token, if the router requires one")
    parser.add_argument("--planner-model", default=PLANNER_MODEL,
                        help="model for the planner leg (default: %(default)s)")
    parser.add_argument("--worker-model", default=WORKER_MODEL,
                        help="model for the concurrent sub-agents "
                             "(default: %(default)s)")
    parser.add_argument("--label", default="", help="printed with the summary")
    args = parser.parse_args()

    if args.policy:
        try:
            active = set_policy(args.router, args.policy, args.token)
            print(f"policy: {active.get('policy')}", file=sys.stderr)
        except Exception as exc:  # noqa: BLE001
            print(f"could not set policy: {exc}", file=sys.stderr)
            return 2

    running = active_policy(args.router, args.token)
    if args.policy and running and running != args.policy:
        print(f"router reports policy '{running}', expected '{args.policy}'",
              file=sys.stderr)
        return 2

    results, wall = run_scenario(args)

    ok = [r for r in results if r["ok"]]
    failed = [r for r in results if not r["ok"]]
    by_node = {}
    for r in ok:
        by_node[r["node"]] = by_node.get(r["node"], 0) + 1
    cold = sum(1 for r in ok if not r["warm"])

    label = f" [{args.label}]" if args.label else ""
    print()
    print(f"WALL-CLOCK{label}: {wall:.2f} s   ({len(ok)} ok, {len(failed)} failed)")
    print(f"  cold starts: {cold}/{len(ok)}")
    print("  per node:    " + ", ".join(f"{k}={v}" for k, v in sorted(by_node.items())))
    # One machine-readable line, so a comparison script does not have to parse
    # the human-readable block above.
    print("RESULT " + json.dumps({
        "label": args.label, "policy": running or args.policy, "seed": args.seed,
        "rounds": args.rounds, "subagents": args.subagents, "stream": args.stream,
        "capped": args.capped,
        "scenario": args.scenario, "think_ms": args.think_ms,
        "wall_s": round(wall, 3), "ok": len(ok), "failed": len(failed),
        "cold_starts": cold, "by_node": by_node,
    }))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
