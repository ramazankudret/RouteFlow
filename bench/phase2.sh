#!/usr/bin/env bash
# RouteFlow — Phase 2 comparison: learned cost model against static (§9).
#
# Both arms run the same scenario with the same policy (Warmth) on the same
# cluster. The only difference is which cost model scores the candidates, which
# is the whole point: Phase 1 established that Warmth beats RoundRobin, so
# Phase 2 has to show that a *better estimate* beats a seeded one, not that
# routing beats not routing.
#
# The learned arm is given a warm-up trace to learn from, written by an earlier
# campaign and replayed at startup via --replay_from. It never learns from the
# run being measured on its own output — that would let the arm improve during
# the measurement and make the number meaningless.
#
# §9 asks for both: lower prediction error AND a wall-clock improvement. Either
# alone is not the criterion.
#
#   bench/phase2.sh --runs 5 --rounds 5 --warmup 8

set -euo pipefail

RUNS=5
ROUNDS=5
WARMUP=8
SUBAGENTS=4
SEED=7
BIN="${HOME}/rf-build"
OUT="bench/results-phase2"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --runs)      RUNS="$2"; shift 2 ;;
    --rounds)    ROUNDS="$2"; shift 2 ;;
    --warmup)    WARMUP="$2"; shift 2 ;;
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

# The bracket keeps the pattern from matching this script's own command line.
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

# $1 trace path, $2 cost model, $3 replay path ("" for none), $4 rounds, $5 label
run_once() {
  local trace="$1" cost="$2" replay="$3" rounds="$4" label="$5"
  cleanup; sleep 1
  start_agents
  rm -f "${trace}"

  local replay_args=(--replay_trace false)
  if [[ -n "${replay}" ]]; then
    replay_args=(--replay_from "${replay}")
  fi

  "${BIN}/routeflow-router" --config "${REPO}/bench/router.json" \
      --nodes "${OUT}/nodes.json" --trace "${trace}" \
      --policy warmth-v1 --cost_model "${cost}" "${replay_args[@]}" \
      --log.level warn >/dev/null 2>&1 &
  local pid=$!
  sleep 2

  for _ in $(seq 1 20); do
    local ready
    ready=$(curl -s -m 2 http://127.0.0.1:8970/api/nodes \
            | grep -o '"engine_healthy":true' | wc -l || echo 0)
    [[ "${ready}" -ge 3 ]] && break
    sleep 0.5
  done

  python3 "${REPO}/bench/loadgen.py" --router http://127.0.0.1:8970 \
      --rounds "${rounds}" --subagents "${SUBAGENTS}" --seed "${SEED}" \
      --label "${label}" 2>/dev/null | tee /dev/stderr \
      | grep '^RESULT ' | sed 's/^RESULT //' >> "${OUT}/results.jsonl" || true

  kill "${pid}" 2>/dev/null || true
  wait "${pid}" 2>/dev/null || true
}

echo "warm-up: ${WARMUP} rounds, static cost model, to give the learned arm a past"
: > "${OUT}/results.jsonl"
run_once "${OUT}/warmup.jsonl" static-v1 "" "${WARMUP}" "warmup"
echo

for run in $(seq 1 "${RUNS}"); do
  echo "=== static-v1 run ${run}/${RUNS} ==="
  run_once "${OUT}/static-v1-run${run}.jsonl" static-v1 "" "${ROUNDS}" "static-v1-run${run}"
  echo
  echo "=== learned-v1 run ${run}/${RUNS} ==="
  run_once "${OUT}/learned-v1-run${run}.jsonl" learned-v1 "${OUT}/warmup.jsonl" \
           "${ROUNDS}" "learned-v1-run${run}"
  echo
done

cleanup
echo
python3 "${REPO}/bench/summarize.py" "${OUT}" \
        --baseline static-v1 --policy learned-v1
echo "traces in ${OUT}/"
