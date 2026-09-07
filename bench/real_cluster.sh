#!/usr/bin/env bash
# A RouteFlow cluster on a real network, and what happens when a node leaves it.
#
# Every result in docs/ so far was measured over 127.0.0.1, where a connection
# either succeeds immediately or is refused immediately. The router has a whole
# layer for the other cases -- NodeStale admission, poll_failures, retry to
# another node, and §6.4's rule that once a byte has reached the client there is
# no retry -- and none of it had ever seen a timeout, a partition or a slow
# link. This runs it on a Docker bridge, where all three are producible.
#
# Each container is one machine: the engine bound to its own loopback, the agent
# in front of it as the only reachable port, authenticating (D18). The router is
# a container on the same network, so the router-agent hop is a real TCP hop
# across a real bridge rather than a loopback shortcut.
#
#   bench/real_cluster.sh up              build the cluster and leave it running
#   bench/real_cluster.sh campaign        warmth vs roundrobin, over the network
#   bench/real_cluster.sh faults          partition / freeze / kill, with asserts
#   bench/real_cluster.sh latency MS      add MS of one-way delay to node-b
#   bench/real_cluster.sh down            remove everything

set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

BIN="${HOME}/rf-build"
NET=rf-net
TOKEN=cluster-secret-token
CLIENT_TOKEN=client-secret-token
ROUTER_PORT=8990
OUT=bench/results-real-cluster
# Node A gets the card. Node B is CPU-only, which is the heterogeneity Phase 1
# needs; bench/real_cluster.sh ratio builds the other shape.
NODES=(a b)

log() { echo "  $*"; }

# --- lifecycle ---------------------------------------------------------------

up() {
  mkdir -p "${OUT}"
  docker network inspect "${NET}" >/dev/null 2>&1 || {
    log "creating network ${NET}"
    docker network create "${NET}" >/dev/null
  }

  start_node a "--gpus all"
  start_node b ""

  # nodes.json names containers, not addresses: Docker's own DNS resolves them
  # on this network, which is one more thing loopback never exercised.
  cat > "${OUT}/nodes.json" <<JSON
{ "nodes": [
    { "id": "a", "endpoint": "rf-node-a:8971" },
    { "id": "b", "endpoint": "rf-node-b:8971" }
] }
JSON

  wait_for_agents
}

start_node() {   # $1 id, $2 extra docker args
  local id="$1" extra="$2"
  docker rm -f "rf-node-${id}" >/dev/null 2>&1 || true
  # shellcheck disable=SC2086
  if ! docker run -d --name "rf-node-${id}" --network "${NET}" ${extra} \
    --cap-add NET_ADMIN \
    -e RF_NODE_ID="${id}" -e RF_TOKEN="${TOKEN}" \
    -e RF_TELEMETRY="$( [[ -n "${extra}" ]] && echo nvml || echo null )" \
    -v rf_models:/root/.ollama \
    -v "${HOME}/rf-build:/rf:ro" \
    routeflow-node:latest >/dev/null; then
    # A container that failed to start is not a slow container; letting the
    # readiness loop call it "never became ready" hides docker's own error.
    echo "could not start node ${id}" >&2
    exit 4
  fi
  log "node ${id} started${extra:+ (}${extra}${extra:+)}"
}

wait_for_agents() {
  local id ok
  for id in "${NODES[@]}"; do
    ok=0
    for _ in $(seq 1 90); do
      if docker exec "rf-node-${id}" curl -s -m 2 \
           -H "Authorization: Bearer ${TOKEN}" \
           http://127.0.0.1:8971/state >/dev/null 2>&1; then
        ok=1; break
      fi
      sleep 1
    done
    if [[ "${ok}" -ne 1 ]]; then
      echo "node ${id} never became ready; logs:" >&2
      docker logs --tail 20 "rf-node-${id}" >&2
      exit 4
    fi
    log "node ${id} ready"
  done
}

start_router() {   # $1 policy, $2 nodes file, $3 trace name (default = policy)
  local nodes_file="${2:-nodes.json}"
  local trace_name="${3:-$1}"
  docker rm -f rf-router >/dev/null 2>&1 || true
  docker run -d --name rf-router --network "${NET}" \
    -p "${ROUTER_PORT}:8990" \
    -v "${HOME}/rf-build:/rf:ro" \
    -v "$(pwd)/${OUT}:/out" \
    --entrypoint /rf/routeflow-router \
    routeflow-node:latest \
    --nodes "/out/${nodes_file}" --trace "/out/${trace_name}.jsonl" \
    --http.bind 0.0.0.0 --http.port 8990 \
    --http.token "${CLIENT_TOKEN}" --node.token "${TOKEN}" \
    --policy "${1}" --cost_model learned-v1 \
    --replay_trace false --log.level info >/dev/null

  local n
  for _ in $(seq 1 60); do
    # wc -l always prints a count, so `|| echo 0` would append a second one
    # and the comparison below would be handed two numbers.
    n=$(curl -s -m 2 -H "Authorization: Bearer ${CLIENT_TOKEN}" \
        "http://127.0.0.1:${ROUTER_PORT}/api/nodes" 2>/dev/null \
        | grep -o '"engine_healthy":true' | wc -l)
    [[ "${n}" -ge 2 ]] && { log "router up, ${n} nodes healthy"; return 0; }
    sleep 1
  done
  echo "router never saw both nodes over the network; logs:" >&2
  docker logs --tail 30 rf-router >&2
  exit 4
}

down() {
  docker rm -f rf-router rf-node-a rf-node-b rf-node-fast rf-node-slow >/dev/null 2>&1 || true
  docker network rm "${NET}" >/dev/null 2>&1 || true
  log "cluster removed"
}



# --- a second heterogeneity ratio --------------------------------------------
#
# Every real result so far comes from one cluster shape: a GPU that decodes
# eight to twenty times faster than the alternative. Both of the open caveats
# depend on that ratio being large -- docs/TTFT-DECISION.md declines a TTFT-aware
# objective *because* the warm node is the slow one, and says so.
#
# Two CPU engines with different core counts give a second shape. They are still
# genuinely heterogeneous and still real Ollama, but the gap is a factor of a few
# rather than a factor of ten. One point does not make a curve; this is the
# second point.
#
# What it cannot be: two GPUs. The decode rates here are CPU rates and the loads
# are mmap rather than PCIe, so this probes the *ratio*, not the hardware.

# $1 = "cpu" (two throttled CPU engines) or "gpu" (two engines on the card).
#
# The CPU pair was the first idea and it does not work: measured, 14 cores
# against 1 gives 32.8 against 25.3 tok/s, a ratio of 1.3. A 0.5B model on
# llama.cpp is bound by memory bandwidth, not cores, so core count is not a
# heterogeneity knob at all. That is worth keeping in the script, because it is
# the kind of thing everyone assumes and nobody measures.
#
# The GPU pair is the shape that answers the open question. Two engines on the
# same card decode at the same speed, which is the "the warm node is not slower"
# regime docs/TTFT-DECISION.md says it cannot test -- and its whole conclusion
# rests on the warm node being slower.
ratio_up() {
  mkdir -p "${OUT}"
  docker network inspect "${NET}" >/dev/null 2>&1 || docker network create "${NET}" >/dev/null
  docker rm -f rf-node-fast rf-node-slow >/dev/null 2>&1 || true

  local kind="${1:-gpu}" fast_cpus="${2:-14}" slow_cpus="${3:-1}"
  local id args
  for id in fast slow; do
    if [[ "${kind}" == "gpu" ]]; then
      args="--gpus all"
    else
      args="--cpus ${fast_cpus}"
      [[ "${id}" == "slow" ]] && args="--cpus ${slow_cpus}"
    fi
    # shellcheck disable=SC2086
    # shellcheck disable=SC2086
    docker run -d --name "rf-node-${id}" --network "${NET}" ${args} \
      --cap-add NET_ADMIN \
      -e RF_NODE_ID="${id}" -e RF_TOKEN="${TOKEN}" \
      -e RF_TELEMETRY="$( [[ "${kind}" == "gpu" ]] && echo nvml || echo null )" \
      -e CUDA_VISIBLE_DEVICES="$( [[ "${kind}" == "gpu" ]] && echo 0 || echo "" )" \
      -e OLLAMA_MAX_LOADED_MODELS=1 \
      -v rf_models:/root/.ollama -v "${HOME}/rf-build:/rf:ro" \
      routeflow-node:latest >/dev/null || { echo "could not start ${id}" >&2; exit 4; }
    log "node ${id} started (${args})"
  done

  cat > "${OUT}/nodes-ratio.json" <<JSON
{ "nodes": [
    { "id": "fast", "endpoint": "rf-node-fast:8971" },
    { "id": "slow", "endpoint": "rf-node-slow:8971" }
] }
JSON

  NODES=(fast slow)
  wait_for_agents
}

# The ratio is measured, never assumed: llama.cpp does not scale linearly with
# cores, and a claim about "roughly three times" that nobody checked would be
# the same mistake as the seeded prefill rate that turned out sevenfold wrong.
ratio_measure() {
  local id t0 t1 ms
  echo "  measured decode, one warm request each:"
  for id in fast slow; do
    docker exec "rf-node-${id}" curl -s -m 300 http://127.0.0.1:11434/api/generate       -d '{"model":"qwen2.5:0.5b","prompt":"hi","stream":false}' >/dev/null 2>&1
    t0=$(date +%s%3N)
    docker exec "rf-node-${id}" curl -s -m 300 http://127.0.0.1:11434/api/generate       -d '{"model":"qwen2.5:0.5b","prompt":"Count from 1 to 120, one per line.","stream":false,"options":{"num_predict":120}}'       > "${OUT}/ratio-${id}.json" 2>/dev/null
    t1=$(date +%s%3N)
    ms=$((t1 - t0))
    python3 - "${OUT}/ratio-${id}.json" "${id}" "${ms}" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1], encoding='utf-8'))
    n = d.get('eval_count', 0)
    ns = d.get('eval_duration', 0)
    rate = n / (ns / 1e9) if ns else 0
    print(f"    {sys.argv[2]:<5} {n:>4} tokens in {int(sys.argv[3]):>6} ms wall, "
          f"{rate:6.1f} tok/s decode")
except Exception as e:
    print(f"    {sys.argv[2]:<5} could not measure: {e}")
PY
  done
}


# The TTFT question, asked again where the warm node is not slower.
#
# docs/TTFT-DECISION.md declines a TTFT-aware objective and names its own
# limit: the conclusion holds because avoiding a load means routing to a node
# that decodes eight to twenty times slower. Two engines on the same card
# decode within 8% of each other, so this is that caveat's regime.
#
# What it is not: two cards. They share 8 GB and one SM array, so concurrent
# decode contends in a way two machines would not. It answers "does the trade
# change when the warm node is not slower", not "how fast is a real pair".
ratio_campaign() {   # $1 rounds
  local rounds="${1:-10}" trace="${OUT}/ratio-warmth.jsonl"
  rm -f "${trace}"
  start_router warmth-v1 nodes-ratio.json ratio-warmth

  # Three models across two nodes, on purpose. With two of each, a
  # warmth-aware router simply gives each model a node and nothing is ever
  # evicted again -- measured: 1 cold start in 20 requests, and no warmth
  # question left to ask. The interesting regime is models outnumbering nodes,
  # where something must always lose its place.
  local i m
  echo "  driving ${rounds} rounds of three models across two nodes"
  for i in $(seq 1 "${rounds}"); do
    for m in qwen2.5:0.5b tinyllama:latest smollm2:135m; do
      python3 bench/cluster_probe.py "http://127.0.0.1:${ROUTER_PORT}" \
        "${CLIENT_TOKEN}" 1 "${m}" 120 \
        "Count from 1 to 100, one per line." stream >/dev/null 2>&1
    done
  done

  local cold
  cold=$(python3 -c "
import json, sys
rs = [json.loads(l) for l in open(sys.argv[1], encoding='utf-8') if l.strip()]
print(sum(1 for r in rs if r.get('outcome') == 'ok' and not r.get('was_resident')))
" "${trace}")
  echo "  cold starts in this run: ${cold}"
  if [[ "${cold}" -lt 3 ]]; then
    echo "  INCONCLUSIVE: almost nothing had to load, so there was no warmth" >&2
    echo "  question to answer. 'The objectives agree' would then be a fact" >&2
    echo "  about the workload, not about the cluster." >&2
    exit 3
  fi

  echo
  python3 bench/ttft_frontier.py "two comparable nodes" "${trace}"
}

# --- faults ------------------------------------------------------------------
#
# Each of these is a state the router has code for and had never been in. The
# assertion is not "it survived" but "it did the specific right thing", because
# a request that returns 200 with an empty body and one that returns a clean 503
# both look like survival from far enough away.
#
# The first version of this faulted a fixed node and passed everything. It was
# worthless: the trace showed that node had served zero of the 21 baseline
# requests, so "the cluster still works without it" was true before the fault
# and after it. The fault now lands on whichever node the baseline actually
# used, read out of the trace rather than assumed, and the harness refuses to
# report a result if the baseline did not give it a busy node to kill.

PASS=0
FAIL=0

expect() {   # $1 description, $2 actual, $3 what counts as pass (regex)
  if [[ "$2" =~ $3 ]]; then
    echo "    PASS  $1"
    PASS=$((PASS + 1))
  else
    echo "    FAIL  $1"
    echo "          got: $2"
    FAIL=$((FAIL + 1))
  fi
}

probe() {   # $1 count
  python3 bench/cluster_probe.py "http://127.0.0.1:${ROUTER_PORT}"     "${CLIENT_TOKEN}" "${1}" qwen2.5:0.5b 2>&1 | grep '^PROBE' || echo "PROBE crashed"
}

healthy_count() {
  curl -s -m 5 -H "Authorization: Bearer ${CLIENT_TOKEN}"     "http://127.0.0.1:${ROUTER_PORT}/api/nodes"     | grep -o '"engine_healthy":true' | wc -l
}

busiest_node() {   # $1 trace, $2 = look at only the last N records (0 = all)
  python3 - "$1" "${2:-0}" <<'PY'
import json, sys, collections
rows = []
try:
    for line in open(sys.argv[1], encoding='utf-8'):
        line = line.strip()
        if line:
            r = json.loads(line)
            if r.get('outcome') == 'ok':
                rows.append(r['node_id'])
except OSError:
    pass
tail = int(sys.argv[2])
if tail:
    rows = rows[-tail:]
c = collections.Counter(rows)
print(f"{c.most_common(1)[0][0]} {c.most_common(1)[0][1]}" if c else "none 0")
PY
}

# grep -c prints 0 and exits 1 when it finds nothing, so `|| echo 0` appends a
# second zero, and the arithmetic below then sees two numbers where it wants
# one. Ask for the count once instead.
retry_count() {
  local n
  n=$(grep -c '"retry_of":"' "$1" 2>/dev/null) || true
  echo "${n:-0}"
}

restore() {
  local id
  for id in "${NODES[@]}"; do
    if ! docker exec "rf-node-${id}" curl -s -m 3          -H "Authorization: Bearer ${TOKEN}" http://127.0.0.1:8971/state 2>/dev/null          | grep -q '"engine_healthy":true'; then
      log "node ${id} is not serving; restarting it"
      docker restart "rf-node-${id}" >/dev/null 2>&1 || true
    fi
  done
  wait_for_agents
}

faults() {
  local trace="${OUT}/warmth-v1.jsonl"
  # A previous fault run can leave a node whose engine was killed and whose
  # agent is still answering -- a real state, and one the router handles, but
  # not the state to start from. Breaking a cluster that was already broken
  # measures the last run, not this fault.
  restore
  start_router warmth-v1
  echo
  echo "  baseline, both nodes reachable"
  expect "a healthy cluster serves every request" "$(probe 4)" "ok=4 of 4"

  read -r VICTIM SERVED <<< "$(busiest_node "${trace}" 0)"
  echo "  the baseline put ${SERVED} requests on node ${VICTIM}; that is what gets faulted"
  if [[ "${SERVED}" -lt 3 ]]; then
    echo "  INCONCLUSIVE: no node carried enough of the baseline to be worth killing." >&2
    echo "  Faulting an idle node proves nothing. Fix the workload and re-run." >&2
    exit 3
  fi
  local NODE="rf-node-${VICTIM}"

  # 1. Partition: connects fail at once. Loopback can produce that, but never
  # for a node that was healthy and busy a second earlier.
  echo
  echo "  fault 1: ${NODE} partitioned from the network"
  docker network disconnect "${NET}" "${NODE}" >/dev/null 2>&1
  sleep 8
  expect "the router stops calling the partitioned node healthy"     "healthy=$(healthy_count)" "healthy=1"
  expect "and the surviving node picks up the work it was doing"     "$(probe 3)" "ok=3 of 3"
  docker network connect "${NET}" "${NODE}" >/dev/null 2>&1
  sleep 8
  expect "the node comes back on its own when the link returns"     "healthy=$(healthy_count)" "healthy=2"

  # 2. Freeze: the socket connects and nothing comes back, so the router has to
  # time out rather than be refused. A different code path from a partition.
  echo
  echo "  fault 2: ${NODE} frozen (connects, never answers)"
  docker pause "${NODE}" >/dev/null 2>&1
  sleep 10
  expect "a node that accepts connections and never answers is dropped too"     "healthy=$(healthy_count)" "healthy=1"
  expect "and requests are still served" "$(probe 3)" "ok=3 of 3"
  docker unpause "${NODE}" >/dev/null 2>&1
  sleep 8
  expect "and it recovers when unfrozen" "healthy=$(healthy_count)" "healthy=2"

  # 3. Latency: the router must not mistake a slow link for a dead one.
  # node_stale_ms is 5 s; this adds 300 ms each way to every poll.
  echo
  echo "  fault 3: 300 ms each way on ${NODE}'s link"
  docker exec "${NODE}" tc qdisc add dev eth0 root netem delay 300ms >/dev/null 2>&1
  sleep 8
  expect "a slow node is still a node" "healthy=$(healthy_count)" "healthy=2"
  expect "and the cluster still serves everything" "$(probe 3)" "ok=3 of 3"
  docker exec "${NODE}" tc qdisc del dev eth0 root >/dev/null 2>&1

  # 4. Death under load. §6.4: a request that has sent nothing to the client may
  # be retried elsewhere; one that has already sent a byte may not.
  echo
  echo "  fault 4: ${NODE} killed while the cluster is under load"
  # 400 tokens, so each request is open for many seconds and the kill below is
  # certain to land on one mid-stream. With a 24-token reply the kill fell
  # between requests every time and the retry path was never reached, which the
  # trace showed as zero retry_of records -- a green test covering nothing.
  python3 bench/cluster_probe.py "http://127.0.0.1:${ROUTER_PORT}"     "${CLIENT_TOKEN}" 6 qwen2.5:0.5b 400 > "${OUT}/kill-probe.txt" 2>&1 &
  local probe_pid=$!
  sleep 6
  docker kill "${NODE}" >/dev/null 2>&1
  wait "${probe_pid}"
  expect "no request is lost when the busy node dies mid-flight"     "$(grep '^PROBE' "${OUT}/kill-probe.txt" || echo none)" "ok=6 of 6"
  docker start "${NODE}" >/dev/null 2>&1
  sleep 15
  expect "and a restarted node rejoins" "healthy=$(healthy_count)" "healthy=2"

  # 5. The engine dies, the agent does not. This is the only fault that reaches
  # §6.4's retry branch on purpose: the node still looks healthy for a moment,
  # so the router dispatches, the proxy to the engine fails, and nothing has
  # reached the client yet -- which is exactly the condition under which a retry
  # elsewhere is allowed. Killing the whole container cannot test this, because
  # the node is gone before the router ever picks it.
  echo
  echo "  fault 5: an engine dies mid-request while its agent survives"
  # Whichever node is serving *now*. Faults 1-4 move the traffic around, so the
  # node the baseline used is not necessarily the node about to be dispatched
  # to -- and killing an idle engine tests nothing, which is how the first
  # version of this passed while the trace recorded no retry at all.
  probe 2 >/dev/null
  local CURRENT
  read -r CURRENT _ <<< "$(busiest_node "${trace}" 2)"
  echo "    (traffic is currently on node ${CURRENT}; killing that engine)"
  local before
  before=$(retry_count "${trace}")
  # The window has to be wide enough to aim at. 400 tokens was about two seconds
  # on the GPU and the kill kept landing between requests; 1500 gives roughly
  # six, which is the difference between testing the retry path and hoping.
  python3 bench/cluster_probe.py "http://127.0.0.1:${ROUTER_PORT}"     "${CLIENT_TOKEN}" 3 qwen2.5:0.5b 1500     "Count from 1 to 300, one number per line, nothing else."     > "${OUT}/engine-probe.txt" 2>&1 &
  local ep=$!
  sleep 5
  docker exec "rf-node-${CURRENT}" pkill -9 ollama >/dev/null 2>&1
  wait "${ep}"
  expect "the request survives its engine dying"     "$(grep '^PROBE' "${OUT}/engine-probe.txt" || echo none)" "ok=3 of 3"
  local after
  after=$(retry_count "${trace}")
  expect "and the trace records it as a retry rather than a fresh request (D9)"     "retries_added=$((after - before))" "retries_added=[1-9]"
  # The retry has to land somewhere else. This is the assertion that found D42:
  # the exclusion list was assembled by the dispatcher and never reached
  # admission, so a request whose node had just failed was retried onto the same
  # node and returned 502 with a healthy node sitting admitted beside it. No
  # unit test sees that -- the two halves are correct in isolation.
  expect "and the retry goes to a different node than the one that failed"     "$(python3 bench/retry_check.py "${trace}")" "moved=[1-9][0-9]* same_node=0"
  docker restart "rf-node-${CURRENT}" >/dev/null 2>&1
  sleep 20

  echo
  echo "  ${PASS} passed, ${FAIL} failed"
  [[ "${FAIL}" -eq 0 ]] || exit 1
}

case "${1:-up}" in
  up)     up ;;
  router) start_router "${2:-warmth-v1}" ;;
  faults) faults ;;
  ratio)  ratio_up "${2:-gpu}" "${3:-14}" "${4:-1}"; ratio_measure ;;
  ratio-campaign) ratio_campaign "${2:-10}" ;;
  down)   down ;;
  *)      echo "unknown command: ${1}" >&2; exit 2 ;;
esac
