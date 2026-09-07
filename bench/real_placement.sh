#!/usr/bin/env bash
# Phase 3 on real hardware: reactive placement against the engine's own LRU.
#
# D34 concluded that placement is neither redundant nor essential -- it earns
# its keep only where traffic leaves the node idle long enough to preload into,
# and is inert otherwise. That verdict came entirely from simulated nodes. This
# runs the same comparison against two real engines, at both think times, so the
# condition itself is tested rather than assumed to transfer.
#
# The setup is bench/real_two_node.sh's: a GPU engine and a CPU engine, both
# under OLLAMA_MAX_LOADED_MODELS=1 so eviction is real, and two small models
# alternating so something is always being displaced.
#
# The adapter's load and drop were verified against this Ollama before the
# campaign: /api/generate with an empty prompt returns done_reason "load", and
# with keep_alive 0 returns "unload". A manager that cannot act is worth nothing
# however well it decides.
#
#   bench/real_placement.sh [--runs 5] [--rounds 8] [--think-ms 0]

set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

BIN="${HOME}/rf-build"
OUT=bench/results-real-placement
RUNS=5
ROUNDS=8
THINK_MS=0
GPU_ENGINE=127.0.0.1:11436
CPU_ENGINE=127.0.0.1:11435
GPU_AGENT=8991
CPU_AGENT=8992
ROUTER_PORT=8990

while [[ $# -gt 0 ]]; do
  case "$1" in
    --runs)     RUNS="$2"; shift 2 ;;
    --rounds)   ROUNDS="$2"; shift 2 ;;
    --think-ms) THINK_MS="$2"; shift 2 ;;
    --out)      OUT="$2"; shift 2 ;;
    --bin)      BIN="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

mkdir -p "${OUT}"
cleanup() { pkill -f '[r]outeflow-(agent|router)' 2>/dev/null || true; }
trap cleanup EXIT
cleanup; sleep 1

for e in "${GPU_ENGINE}" "${CPU_ENGINE}"; do
  curl -s -m 5 "http://${e}/api/tags" >/dev/null || { echo "engine ${e} unreachable"; exit 1; }
done

cat > "${OUT}/nodes.json" <<JSON
{ "nodes": [
    { "id": "gpu", "endpoint": "127.0.0.1:${GPU_AGENT}" },
    { "id": "cpu", "endpoint": "127.0.0.1:${CPU_AGENT}" }
] }
JSON

cat > "${OUT}/drive.py" <<'PY'
import json, random, sys, time, urllib.request

router, rounds, seed, think_ms = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
MODELS = ["qwen2.5:0.5b", "tinyllama:latest"]
rng = random.Random(seed)
# Its own stream, so pauses cannot move the request sequence (D12, and the
# defect that lesson came from).
think_rng = random.Random(seed ^ 0x7417)
words = ["context", "state", "plan", "step", "tool", "result", "note", "value"]

started = time.monotonic()
idle = 0.0
ok = failed = 0
for i in range(rounds * len(MODELS)):
    if i and i % len(MODELS) == 0 and think_ms > 0:
        pause = think_ms / 1000.0 * (0.5 + think_rng.random())
        time.sleep(pause)
        idle += pause
    body = json.dumps({
        "model": MODELS[i % len(MODELS)],
        "messages": [{"role": "user", "content":
                      " ".join(rng.choice(words) for _ in range(120))}],
        "max_tokens": rng.choice([32, 48, 64, 96, 128, 192]),
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

print("WALL %.3f ok %d failed %d idle %.1f"
      % (time.monotonic() - started - idle, ok, failed, idle))
PY

start_cluster() {   # $1 placement on/off, $2 trace, $3 replay
  local replay=(--replay_trace false)
  [[ -n "$3" ]] && replay=(--replay_from "$3")

  # Bringing three processes up on fixed ports races with the previous run's
  # teardown, and losing that race once should not kill a twenty-minute
  # campaign. It should also not be papered over: an attempt that fails is
  # reported, and running out of attempts is fatal. The first version polled
  # for ten seconds, gave up, and ran the requests anyway -- sixteen instant
  # refusals, an empty trace, and a "run" that summarize.py averaged over.
  local attempt
  for attempt in 1 2 3; do
    cleanup; sleep $((attempt * 2))
    "${BIN}/routeflow-agent" --node.id gpu --http.port "${GPU_AGENT}" \
        --engine.kind ollama --engine.endpoint "${GPU_ENGINE}" \
        --log.level error >/dev/null 2>&1 &
    "${BIN}/routeflow-agent" --node.id cpu --http.port "${CPU_AGENT}" \
        --engine.kind ollama --engine.endpoint "${CPU_ENGINE}" \
        --telemetry null --log.level error >/dev/null 2>&1 &
    sleep 3
    rm -f "$2"
    # Only placement differs between the arms. Same policy, same cost model, so
    # a difference is attributable to placement and not to something learned
    # along the way.
    "${BIN}/routeflow-router" --nodes "${OUT}/nodes.json" --trace "$2" \
        --http.port "${ROUTER_PORT}" --policy warmth-v1 --cost_model learned-v1 \
        --placement.enabled "$1" --placement.interval_ms 2000 \
        "${replay[@]}" --log.level warn > "${OUT}/router-$1.log" 2>&1 &
    sleep 3

    local ready=0
    for _ in $(seq 1 40); do
      local n
      n=$(curl -s -m 2 "http://127.0.0.1:${ROUTER_PORT}/api/nodes" \
          | grep -o '"engine_healthy":true' | wc -l || echo 0)
      if [[ "${n}" -ge 2 ]]; then ready=1; break; fi
      sleep 0.5
    done
    [[ "${ready}" -eq 1 ]] && return 0
    echo "  cluster did not come up (attempt ${attempt}/3)" >&2
  done

  echo "cluster never came up after 3 attempts. What the router reports:" >&2
  curl -s -m 3 "http://127.0.0.1:${ROUTER_PORT}/api/nodes" >&2 || echo "  (no answer)" >&2
  echo "  --- router log tail ---" >&2
  tail -5 "${OUT}/router-$1.log" >&2 2>/dev/null || true
  exit 4
}

run_arm() {   # $1 placement, $2 trace, $3 replay, $4 label
  start_cluster "$1" "$2" "$3"
  local line
  line=$(python3 "${OUT}/drive.py" "http://127.0.0.1:${ROUTER_PORT}" "${ROUNDS}" 11 \
         "${THINK_MS}" 2>/dev/null | grep '^WALL ')
  echo "  $4: ${line}"
  # A run that served nothing is not a slow run, it is a broken one, and
  # averaging over it would publish a comparison between unequal arms.
  local failed
  failed=$(echo "${line}" | awk '{print $6}')
  if [[ "${failed}" != "0" ]]; then
    echo "  $4 had ${failed} failed request(s); aborting rather than reporting it" >&2
    exit 5
  fi
  # What placement actually did, which the trace cannot show: a preload is not a
  # job and leaves no record of its own.
  curl -s -m 3 "http://127.0.0.1:${ROUTER_PORT}/admin/stats" \
    | python3 -c "
import json, sys
p = json.load(sys.stdin).get('placement')
print('     placement:', json.dumps(p) if p else 'off')
" 2>/dev/null || true
}

echo "warm-up: one pass so the learned model knows both engines"
start_cluster false "${OUT}/warmup.jsonl" ""
python3 "${OUT}/drive.py" "http://127.0.0.1:${ROUTER_PORT}" "${ROUNDS}" 11 0 >/dev/null 2>&1
echo

echo "think time between rounds: ${THINK_MS} ms"
for run in $(seq 1 "${RUNS}"); do
  echo "run ${run}/${RUNS}"
  run_arm false "${OUT}/lru-run${run}.jsonl"       "${OUT}/warmup.jsonl" "lru-run${run}"
  run_arm true  "${OUT}/placement-run${run}.jsonl" "${OUT}/warmup.jsonl" "placement-run${run}"
done

cleanup
echo
python3 bench/summarize.py "${OUT}" --baseline lru --policy placement
