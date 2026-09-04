#!/usr/bin/env python3
"""RouteFlow load generator — the Phase 1 scenario (ARCHITECTURE §9).

Replays a fixed multi-agent workload: each round is one planner call on a large
model followed by several concurrent sub-agent calls on a small one. That shape
is the point. A single-model workload has no warmth to be aware of, and a
workload with no concurrency never exercises the queue or contention terms.

Reproducibility is a requirement, not a nicety (D12): prompt sizes and output
lengths come from a seeded RNG, and `max_tokens` is always set explicitly so a
simulated node performs exactly the modelled amount of work. Two runs with the
same seed issue byte-identical requests.

  python3 bench/loadgen.py --router http://127.0.0.1:8970 --rounds 6
  python3 bench/loadgen.py --policy warmth-v1 --rounds 6 --seed 42

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

PLANNER_MODEL = "planner:12b"
WORKER_MODEL = "worker:3b"


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
         stream=True):
    # Streaming by default, because that is what agent tooling does and because
    # time-to-first-token only exists for a stream: on a buffered reply the
    # first byte is the whole answer, so the router records ttft as null rather
    # than as a number describing something else. A non-streamed bench run
    # reports no TTFT at all, which §9 asks Phase 1 to publish.
    payload = {
        "model": model,
        "messages": [{"role": "user", "content": make_prompt(rng_stream, prompt_tokens)}],
        "max_tokens": output_tokens,
        "stream": stream,
    }
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


def run_scenario(args):
    rng = random.Random(args.seed)
    results = []
    started = time.monotonic()

    for round_index in range(args.rounds):
        # The planner thinks first; the sub-agents fan out from its output. That
        # dependency is what makes the workload alternate models, which is what
        # puts a node with too little VRAM under eviction pressure.
        planner = call(args.router, PLANNER_MODEL, "planner",
                       rng.randint(900, 1500), rng.randint(110, 170),
                       args.token, args.timeout, rng, args.stream)
        results.append(planner)
        if not planner["ok"]:
            print(f"  round {round_index + 1}: planner failed: {planner['error']}",
                  file=sys.stderr)

        jobs = [(rng.randint(300, 600), rng.randint(40, 90))
                for _ in range(args.subagents)]
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.subagents) as pool:
            futures = [pool.submit(call, args.router, WORKER_MODEL, "subagent",
                                   prompt, output, args.token, args.timeout,
                                   random.Random(args.seed + round_index * 100 + i),
                                   args.stream)
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
    parser.add_argument("--no-stream", dest="stream", action="store_false",
                        help="send buffered requests; TTFT is then unmeasurable")
    parser.set_defaults(stream=True)
    parser.add_argument("--token", help="bearer token, if the router requires one")
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
        "wall_s": round(wall, 3), "ok": len(ok), "failed": len(failed),
        "cold_starts": cold, "by_node": by_node,
    }))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
