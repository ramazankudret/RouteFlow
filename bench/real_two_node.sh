#!/usr/bin/env bash
# Phase 1's question, on real hardware, without a second GPU.
#
# The premise this project exists for: is it better to send a request to the
# fast node that must spend seconds loading the model, or to the slower node
# that already holds it warm? Every measurement of that so far has been against
# simulated nodes, because it appears to need two GPUs.
#
# It does not. Two Ollama instances on one machine -- one with the card, one on
# the CPU -- are genuinely heterogeneous and genuinely independent: separate
# processes, separate memory, separate resident sets. Measured before building
# this (steady state, two small models alternating under
# OLLAMA_MAX_LOADED_MODELS=1):
#
#     GPU   load 2.5-5.4 s   decode 230-274 tok/s
#     CPU   load 1.0-1.7 s   decode  12- 28 tok/s
#
# The GPU decodes an order of magnitude faster and loads two to three times
# slower, because it has to move weights across PCIe while the CPU engine just
# maps them. So the trade-off is live and the crossover sits at roughly
#
#     4000 + 4N  =  50N   ->   N ~ 87 tokens
#
# Below that a warm CPU beats a cold GPU; above it the GPU wins even paying for
# the load. Output lengths here straddle that line on purpose, so neither
# answer is right for every request and the policy has to actually decide.
#
#   bench/real_two_node.sh [--runs 5] [--rounds 8]

set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

BIN="${HOME}/rf-build"
OUT=bench/results-real-two-node
RUNS=5
ROUNDS=8
GPU_ENGINE=127.0.0.1:11436
CPU_ENGINE=127.0.0.1:11435
GPU_AGENT=8991
CPU_AGENT=8992
ROUTER_PORT=8990

# --nodes points the campaign at a cluster that is already running: agents you
# started yourself, on whatever machines you have. That is the one way the open
# question in docs/REAL-TWO-NODE-RESULTS.md gets answered -- every result here
# comes from a card paired with a CPU engine, and nobody has run it against two
# real GPUs because this box has one.
NODES_FILE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --runs)   RUNS="$2"; shift 2 ;;
    --rounds) ROUNDS="$2"; shift 2 ;;
    --bin)    BIN="$2"; shift 2 ;;
    --nodes)  NODES_FILE="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ -n "${NODES_FILE}" && ! -r "${NODES_FILE}" ]]; then
  echo "cannot read ${NODES_FILE}" >&2
  exit 2
fi

mkdir -p "${OUT}"
# The router is what is under test, so it is restarted for every run. The
# agents are not: they report facts about a node -- how fast it loads, how many
# models its engine keeps -- that hold whatever policy is running, and killing
# them throws those measurements away.
#
# That was not free. The agent works out an engine's resident-model limit by
# watching for a model dropped before its expiry (D38), and at a 1 Hz poll
# against sub-second requests it misses most swaps and needs about six to
# converge. Restarting it every run meant roughly a quarter of each run was
# scored against an unknown limit: 22 of 80 requests were priced at zero load
# and cost 2,117 ms at the median. A production agent runs for hours and pays
# that window once; this harness was paying it ten times.
cleanup_all() { pkill -f '[r]outeflow-(agent|router)' 2>/dev/null || true; }
cleanup() { pkill -f '[r]outeflow-router' 2>/dev/null || true; }
# An EXIT trap that returns normally hands bash the trap's status, not the
# script's, so every `exit N` below was reported to the caller as 0 -- the
# refusal printed and the harness looked like it had passed. Re-exiting with
# the saved status is the fix.
trap 'rc=$?; cleanup_all; exit ${rc}' EXIT
cleanup_all; sleep 1

if [[ -z "${NODES_FILE}" ]]; then
  for e in "${GPU_ENGINE}" "${CPU_ENGINE}"; do
    curl -s -m 5 "http://${e}/api/tags" >/dev/null || { echo "engine ${e} unreachable"; exit 1; }
  done
fi

if [[ -n "${NODES_FILE}" ]]; then
  cp "${NODES_FILE}" "${OUT}/nodes.json"
  echo "using the cluster described by ${NODES_FILE}:"
  python3 -c "
import json, sys
for n in json.load(open(sys.argv[1]))['nodes']:
    print('  %-12s %s' % (n['id'], n['endpoint']))
" "${OUT}/nodes.json"
else
  cat > "${OUT}/nodes.json" <<JSON
{ "nodes": [
    { "id": "gpu", "endpoint": "127.0.0.1:${GPU_AGENT}" },
    { "id": "cpu", "endpoint": "127.0.0.1:${CPU_AGENT}" }
] }
JSON
fi

# The workload. Two models so that MAX_LOADED_MODELS=1 forces real evictions,
# and output lengths spanning the crossover so the right answer changes from
# request to request. Seeded, so both policies see identical traffic (D12).
cat > "${OUT}/drive.py" <<'PY'
import json, random, sys, time, urllib.request

router, rounds, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
MODELS = ["qwen2.5:0.5b", "tinyllama:latest"]
rng = random.Random(seed)
words = ["context", "state", "plan", "step", "tool", "result", "note", "value"]

started = time.monotonic()
ok = failed = 0
for i in range(rounds * len(MODELS)):
    model = MODELS[i % len(MODELS)]          # alternating: forces eviction
    out_tokens = rng.choice([32, 48, 64, 96, 128, 192])   # straddles ~87
    body = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content":
                      " ".join(rng.choice(words) for _ in range(120))}],
        "max_tokens": out_tokens,
        "stream": True,
    }).encode()
    req = urllib.request.Request(router + "/v1/chat/completions", data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("X-RouteFlow-Role", "subagent")
    try:
        with urllib.request.urlopen(req, timeout=900) as r:
            r.read()
        ok += 1
    except Exception as exc:
        failed += 1
        print("  request %d failed: %s" % (i, exc), file=sys.stderr)

wall = time.monotonic() - started
print("WALL %.3f ok %d failed %d" % (wall, ok, failed))
PY

start_agents() {
  # Somebody else's cluster: they started the agents, and this campaign has no
  # business managing them.
  [[ -n "${NODES_FILE}" ]] && return 0
  pgrep -f '[r]outeflow-agent' >/dev/null && return 0   # already watching
  # The GPU node reads NVML. The CPU node has no VRAM to report, so it says so
  # rather than borrowing the card's numbers -- admission then treats the engine
  # as the authority, which is exactly right for a node whose memory is RAM.
  "${BIN}/routeflow-agent" --node.id gpu --http.port "${GPU_AGENT}" \
      --engine.kind ollama --engine.endpoint "${GPU_ENGINE}" \
      --log.level error >/dev/null 2>&1 &
  "${BIN}/routeflow-agent" --node.id cpu --http.port "${CPU_AGENT}" \
      --engine.kind ollama --engine.endpoint "${CPU_ENGINE}" \
      --telemetry null --log.level error >/dev/null 2>&1 &
  sleep 3
}

start_cluster() {   # $1 policy, $2 trace, $3 replay ("" for none)
  cleanup; sleep 1
  start_agents
  local replay=(--replay_trace false)
  [[ -n "$3" ]] && replay=(--replay_from "$3")
  rm -f "$2"
  # learned-v1 in both arms. The seed table is keyed on a GPU name and the CPU
  # node reports none, so a seeded model cannot tell these two nodes apart --
  # which would hand Warmth a handicap that has nothing to do with warmth.
  "${BIN}/routeflow-router" --nodes "${OUT}/nodes.json" --trace "$2" \
      --http.port "${ROUTER_PORT}" --policy "$1" --cost_model learned-v1 \
      "${replay[@]}" --log.level warn > "${OUT}/router.log" 2>&1 &
  sleep 3
  for _ in $(seq 1 20); do
    local n
    n=$(curl -s -m 2 "http://127.0.0.1:${ROUTER_PORT}/api/nodes" \
        | grep -o '"engine_healthy":true' | wc -l || echo 0)
    [[ "${n}" -ge 2 ]] && break
    sleep 0.5
  done
}

run_arm() {   # $1 policy, $2 trace, $3 replay, $4 label
  start_cluster "$1" "$2" "$3"
  local line
  line=$(python3 "${OUT}/drive.py" "http://127.0.0.1:${ROUTER_PORT}" "${ROUNDS}" 11 \
         2>/dev/null | grep '^WALL ')
  echo "  $4: ${line}"
  echo "{\"label\":\"$4\",\"policy\":\"$1\",\"$(echo ${line} | awk '{print "wall_s\":"$2",\"ok\":"$4",\"failed\":"$6}')}" \
    >> "${OUT}/results.jsonl"
}

: > "${OUT}/results.jsonl"

echo "warm-up: one pass to give the learned model both nodes' rates"
run_arm warmth-v1 "${OUT}/warmup.jsonl" "" "warmup"
echo

for run in $(seq 1 "${RUNS}"); do
  echo "run ${run}/${RUNS}"
  run_arm roundrobin-v1 "${OUT}/roundrobin-v1-run${run}.jsonl" "${OUT}/warmup.jsonl" "roundrobin-v1-run${run}"
  run_arm warmth-v1     "${OUT}/warmth-v1-run${run}.jsonl"     "${OUT}/warmup.jsonl" "warmth-v1-run${run}"
done

cleanup
echo
python3 bench/summarize.py "${OUT}" --baseline roundrobin-v1 --policy warmth-v1
