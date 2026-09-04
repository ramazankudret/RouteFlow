#!/usr/bin/env bash
# RouteFlow — Phase 1 policy comparison (ARCHITECTURE §9, D12).
#
# Runs the same scenario against each policy N times and reports total scenario
# wall-clock, median across runs. Wall-clock is the metric Phase 1 is decided
# on; p50/p95 latency and prediction error are reported alongside but are not
# decisive, because routing to a warm weak node can improve p50 while damaging
# p95 and the honest question for an agent workload is when the batch finished.
#
# Every run starts from an identical cluster state: the simulated agents are
# restarted between runs so model residency resets. Without that, run 2 of a
# policy inherits run 1's warm cache and the comparison measures ordering
# instead of policy.
#
#   bench/compare.sh --runs 5 --rounds 6 --bin ~/rf-build

set -euo pipefail

RUNS=5
ROUNDS=6
SUBAGENTS=4
SEED=7
BIN="${HOME}/rf-build"
OUT="bench/results"
POLICIES=("roundrobin-v1" "warmth-v1")

while [[ $# -gt 0 ]]; do
  case "$1" in
    --runs)      RUNS="$2"; shift 2 ;;
    --rounds)    ROUNDS="$2"; shift 2 ;;
    --subagents) SUBAGENTS="$2"; shift 2 ;;
    --seed)      SEED="$2"; shift 2 ;;
    --bin)       BIN="$2"; shift 2 ;;
    --out)       OUT="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILES="${REPO}/bench/profiles"
mkdir -p "${OUT}"

# The bracket keeps the pattern from matching this script's own command line —
# pkill -f matches the full cmdline, and a plain 'routeflow-' pattern makes the
# script kill the shell running it. Silently, with no output at all.
cleanup() { pkill -f '[r]outeflow-(agent|router)' 2>/dev/null || true; }
trap cleanup EXIT
cleanup; sleep 1

start_agents() {
  "${BIN}/routeflow-agent" --simulate "${PROFILES}/sim-desktop.json" \
      --http.port 8981 --node.id sim-desktop --log.level warn >/dev/null 2>&1 &
  "${BIN}/routeflow-agent" --simulate "${PROFILES}/sim-jetson.json" \
      --http.port 8982 --node.id sim-jetson --log.level warn >/dev/null 2>&1 &
  "${BIN}/routeflow-agent" --simulate "${PROFILES}/sim-laptop.json" \
      --http.port 8983 --node.id sim-laptop --log.level warn >/dev/null 2>&1 &
  sleep 2
}

cat > "${OUT}/nodes.json" <<'JSON'
{ "nodes": [
    { "id": "sim-desktop", "endpoint": "127.0.0.1:8981" },
    { "id": "sim-jetson",  "endpoint": "127.0.0.1:8982" },
    { "id": "sim-laptop",  "endpoint": "127.0.0.1:8983" }
] }
JSON

echo "cluster: sim-desktop (8GB, fast, cannot hold both models)"
echo "         sim-jetson  (15GB, slow, holds both)"
echo "         sim-laptop  (6GB, worker only)"
echo "runs: ${RUNS} x ${ROUNDS} rounds x (1 planner + ${SUBAGENTS} sub-agents)"
echo

RESULTS="${OUT}/results.jsonl"
: > "${RESULTS}"

for policy in "${POLICIES[@]}"; do
  for run in $(seq 1 "${RUNS}"); do
    echo "=== ${policy} run ${run}/${RUNS} ==="
    cleanup; sleep 1
    start_agents

    trace="${OUT}/${policy}-run${run}.jsonl"
    rm -f "${trace}"
    "${BIN}/routeflow-router" --config "${REPO}/bench/router.json" \
        --nodes "${OUT}/nodes.json" --trace "${trace}" --policy "${policy}" \
        --log.level warn >/dev/null 2>&1 &
    router_pid=$!
    sleep 2

    # Every node must have reported before the first request, or the earliest
    # rounds route against a half-known cluster and the run measures start-up.
    for _ in $(seq 1 20); do
      ready=$(curl -s -m 2 http://127.0.0.1:8970/api/nodes \
              | grep -o '"engine_healthy":true' | wc -l || echo 0)
      [[ "${ready}" -ge 3 ]] && break
      sleep 0.5
    done

    python3 "${REPO}/bench/loadgen.py" --router http://127.0.0.1:8970 \
        --rounds "${ROUNDS}" --subagents "${SUBAGENTS}" --seed "${SEED}" \
        --label "${policy}-run${run}" 2>/dev/null | tee /dev/stderr \
        | grep '^RESULT ' | sed 's/^RESULT //' >> "${RESULTS}" || true

    kill "${router_pid}" 2>/dev/null || true
    wait "${router_pid}" 2>/dev/null || true
    echo
  done
done

cleanup
echo
python3 - "${RESULTS}" <<'PYEOF'
import json, statistics, sys
from collections import defaultdict

rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
by_policy = defaultdict(list)
for r in rows:
    by_policy[r["policy"]].append(r)

print("=" * 62)
print("PHASE 1 EXIT CRITERION — scenario wall-clock, median of N runs (D12)")
print("=" * 62)
print(f"{'policy':<16} {'runs':>4} {'median s':>10} {'IQR s':>12} {'cold':>6} {'failed':>7}")
print("-" * 62)

medians = {}
for policy, runs in sorted(by_policy.items()):
    walls = sorted(r["wall_s"] for r in runs)
    med = statistics.median(walls)
    medians[policy] = med
    if len(walls) >= 4:
        q1 = statistics.median(walls[: len(walls) // 2])
        q3 = statistics.median(walls[(len(walls) + 1) // 2 :])
        iqr = f"{q1:.1f}-{q3:.1f}"
    else:
        iqr = f"{walls[0]:.1f}-{walls[-1]:.1f}"
    cold = statistics.median([r["cold_starts"] for r in runs])
    failed = sum(r["failed"] for r in runs)
    print(f"{policy:<16} {len(runs):>4} {med:>10.2f} {iqr:>12} {cold:>6.0f} {failed:>7}")

if "roundrobin-v1" in medians and "warmth-v1" in medians:
    base, warm = medians["roundrobin-v1"], medians["warmth-v1"]
    delta = (base - warm) / base * 100.0
    print("-" * 62)
    verdict = "Warmth WINS" if warm < base else "Warmth LOSES"
    print(f"{verdict}: {delta:+.1f}% wall-clock vs RoundRobin")
    if warm >= base:
        print()
        print("A loss here is a result, not a bug. ARCHITECTURE §9: if Warmth does")
        print("not win on a heterogeneous cluster the premise is wrong, and that is")
        print("worth knowing immediately. Check the decision breakdowns in the trace")
        print("before concluding either way.")
PYEOF

echo
echo "traces in ${OUT}/  — per-run detail:"
echo "  python3 bench/trace_report.py ${OUT}/warmth-v1-run1.jsonl"
