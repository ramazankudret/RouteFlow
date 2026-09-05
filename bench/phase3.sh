#!/usr/bin/env bash
# RouteFlow — Phase 3 comparison: reactive placement against LRU (§9).
#
# The baseline is not "no policy" — it is the engine managing its own residency,
# which is LRU: Ollama loads on demand and drops the least-recently-used model
# when it needs the room. That is a real strategy and a decent one, which is why
# §9 names it as the thing to beat rather than comparing against nothing.
#
# Both arms run Warmth with the static cost model. Only placement differs, so a
# difference is attributable to placement and not to something learned along the
# way.
#
# The scenario is NOT the one Phases 1 and 2 used. That workload has two models
# and a node with room for both, so every model with demand is already resident
# and there is nothing to preload — measured, not assumed: the manager skipped
# every cycle and cold starts were identical at 3. Placement only means anything
# when the working set does not fit, so this uses four models on nodes that hold
# two, accessed with a skew. That is the ordinary situation on a local cluster
# with more models than memory, and it is the pattern that defeats LRU: the
# rotating models sweep out the hot one that is about to be needed again.
#
# §9's exit criterion has two halves and the second is a guard, not a bonus:
# cold starts must fall substantially AND p95 must not regress. A manager that
# preloads aggressively can always cut cold starts — by stealing the capacity
# that served the requests, which shows up in the tail.
#
#   bench/phase3.sh --runs 5 --rounds 5

set -euo pipefail

RUNS=5
ROUNDS=5
SUBAGENTS=4
SEED=7
THINK_MS=0
BIN="${HOME}/rf-build"
OUT="bench/results-phase3"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --runs)      RUNS="$2"; shift 2 ;;
    --rounds)    ROUNDS="$2"; shift 2 ;;
    --subagents) SUBAGENTS="$2"; shift 2 ;;
    --seed)      SEED="$2"; shift 2 ;;
    --think-ms)  THINK_MS="$2"; shift 2 ;;
    --bin)       BIN="$2"; shift 2 ;;
    --out)       OUT="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILES="${REPO}/bench/profiles"
mkdir -p "${OUT}"

cleanup() { pkill -f '[r]outeflow-(agent|router)' 2>/dev/null || true; }
trap cleanup EXIT
cleanup; sleep 1

# The pressure profiles, not the Phase 1/2 cluster. The first version started
# sim-desktop and sim-jetson here while nodes.json labelled their ports
# pressure-a and pressure-b, so every trace carried pressure node names over the
# agent workload, and the campaign measured the scenario this script's own
# header says it is not using.
start_agents() {
  "${BIN}/routeflow-agent" --simulate "${PROFILES}/pressure-a.json" \
      --http.port 8981 --node.id pressure-a --log.level warn >/dev/null 2>&1 &
  "${BIN}/routeflow-agent" --simulate "${PROFILES}/pressure-b.json" \
      --http.port 8982 --node.id pressure-b --log.level warn >/dev/null 2>&1 &
  sleep 2
}

# Two mislabelled ports were enough to run the wrong experiment for a whole
# campaign and report it as this one, so the cluster is checked before anything
# is measured rather than trusted because the file names look right.
verify_cluster() {
  local models
  models=$(curl -s -m 3 http://127.0.0.1:8970/api/nodes \
           | grep -o '"name":"[^"]*"' | sort -u | tr '\n' ' ')
  case "${models}" in
    *hot:4b*) : ;;
    *) echo "not the pressure cluster: ${models}" >&2
       echo "expected hot:4b and cold-*:4b from bench/profiles/pressure-*.json" >&2
       exit 3 ;;
  esac
}

cat > "${OUT}/nodes.json" <<'JSON'
{ "nodes": [
    { "id": "pressure-a", "endpoint": "127.0.0.1:8981" },
    { "id": "pressure-b", "endpoint": "127.0.0.1:8982" }
] }
JSON

# $1 trace, $2 placement on/off, $3 label
run_once() {
  local trace="$1" placement="$2" label="$3"
  cleanup; sleep 1
  start_agents
  rm -f "${trace}"

  "${BIN}/routeflow-router" --config "${REPO}/bench/router.json" \
      --nodes "${OUT}/nodes.json" --trace "${trace}" \
      --policy warmth-v1 --cost_model static-v1 --replay_trace false \
      --placement.enabled "${placement}" --placement.interval_ms 3000 \
      --log.level warn >/dev/null 2>&1 &
  local pid=$!
  sleep 2

  for _ in $(seq 1 20); do
    local ready
    ready=$(curl -s -m 2 http://127.0.0.1:8970/api/nodes \
            | grep -o '"engine_healthy":true' | wc -l || echo 0)
    [[ "${ready}" -ge 2 ]] && break
    sleep 0.5
  done

  verify_cluster

  # --scenario pressure is the whole point of this phase and it was
  # missing: the default is the agent workload, so the campaign re-ran
  # Phases 1 and 2 under Phase 3 labels.
  python3 "${REPO}/bench/loadgen.py" --router http://127.0.0.1:8970 \
      --scenario pressure --think-ms "${THINK_MS}" \
      --rounds "${ROUNDS}" --subagents "${SUBAGENTS}" --seed "${SEED}" \
      --label "${label}" 2>/dev/null | tee /dev/stderr \
      | grep '^RESULT ' | sed 's/^RESULT //' >> "${OUT}/results.jsonl" || true

  # What placement actually did, which the trace cannot show: a preload is not
  # a job and leaves no record of its own.
  python3 "${REPO}/bench/placement_stats.py" http://127.0.0.1:8970 || true

  kill "${pid}" 2>/dev/null || true
  wait "${pid}" 2>/dev/null || true
}

: > "${OUT}/results.jsonl"
for run in $(seq 1 "${RUNS}"); do
  echo "=== lru run ${run}/${RUNS} (placement off — the engine's own LRU) ==="
  run_once "${OUT}/lru-run${run}.jsonl" false "lru-run${run}"
  echo
  echo "=== placement run ${run}/${RUNS} ==="
  run_once "${OUT}/placement-run${run}.jsonl" true "placement-run${run}"
  echo
done

cleanup
echo
python3 "${REPO}/bench/summarize.py" "${OUT}" --baseline lru --policy placement
echo
echo "cold starts, split into the kind placement can remove and the kind it cannot:"
python3 "${REPO}/bench/cold_starts.py" "${OUT}/lru-run"*.jsonl
echo
python3 "${REPO}/bench/cold_starts.py" "${OUT}/placement-run"*.jsonl
echo "traces in ${OUT}/"
