#!/usr/bin/env bash
# Bring up a cluster and the operator console, and leave them running.
#
# This is the "look at it" script, not a measurement one: three simulated nodes,
# a router with a warm cost model, and enough traffic that every screen has
# something on it. Nothing here writes a result anybody should quote -- for that
# see bench/compare.sh and the rest.
#
#   bench/console.sh              simulated nodes, console on the WSL address
#   bench/console.sh --rounds 10  more traffic before it hands back the URL
#   bench/console.sh --stop       shut it all down
#
# Why it binds to this machine's address rather than loopback: the console has
# to be reachable from a browser outside WSL, and the router refuses any
# non-loopback bind without a token (§10). So it gets one, and the URL carries
# it -- ui/rf-bind.js reads ?token= once and keeps it for the session.

set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

BIN="${HOME}/rf-build"
OUT="${RF_CONSOLE_DIR:-/tmp/rf-console}"
TOKEN="${RF_CONSOLE_TOKEN:-console-view-token}"
PORT=8970
ROUNDS=6
SUBAGENTS=4

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rounds)    ROUNDS="$2"; shift 2 ;;
    --subagents) SUBAGENTS="$2"; shift 2 ;;
    --bin)       BIN="$2"; shift 2 ;;
    --stop)
      pkill -f '[r]outeflow-(agent|router)' 2>/dev/null
      sleep 1
      pgrep -f '[r]outeflow-' >/dev/null && { echo "still running"; exit 1; }
      echo "console stopped"
      exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

for b in routeflow-agent routeflow-router; do
  [[ -x "${BIN}/${b}" ]] || { echo "no ${b} in ${BIN} -- build first" >&2; exit 2; }
done

IP=$(hostname -I | awk '{print $1}')
mkdir -p "${OUT}"

pkill -f '[r]outeflow-(agent|router)' 2>/dev/null
sleep 1

# setsid: a background process started from `wsl bash -lc` dies with the
# invocation that started it, and the point of this script is to leave
# something running.
start() { setsid nohup "$@" >/dev/null 2>&1 </dev/null & }

echo "  starting three simulated nodes"
start "${BIN}/routeflow-agent" --simulate bench/profiles/sim-desktop.json \
      --http.port 8981 --node.id sim-desktop --log.level error
start "${BIN}/routeflow-agent" --simulate bench/profiles/sim-jetson.json \
      --http.port 8982 --node.id sim-jetson --log.level error
start "${BIN}/routeflow-agent" --simulate bench/profiles/sim-laptop.json \
      --http.port 8983 --node.id sim-laptop --log.level error
sleep 3

cat > "${OUT}/nodes.json" <<JSON
{ "nodes": [
    { "id": "sim-desktop", "endpoint": "127.0.0.1:8981" },
    { "id": "sim-jetson",  "endpoint": "127.0.0.1:8982" },
    { "id": "sim-laptop",  "endpoint": "127.0.0.1:8983" }
] }
JSON

# Replayed from a finished campaign so the cost model opens with learned rates.
# Against a cold model the accuracy screen has nothing to draw and the decision
# screen reports every choice as seeded, which is true and useless to look at.
echo "  starting the router"
setsid nohup "${BIN}/routeflow-router" \
    --nodes "${OUT}/nodes.json" --trace "${OUT}/console.jsonl" \
    --replay_from bench/results-phase2/learned-v1-run5.jsonl \
    --http.bind "${IP}" --http.port "${PORT}" --http.token "${TOKEN}" \
    --policy warmth-v1 --cost_model learned-v1 \
    --log.level info > "${OUT}/router.log" 2>&1 </dev/null &
sleep 4

healthy=0
for _ in $(seq 1 40); do
  healthy=$(curl -s -m 2 -H "Authorization: Bearer ${TOKEN}" \
      "http://${IP}:${PORT}/api/nodes" | grep -o '"engine_healthy":true' | wc -l)
  [[ "${healthy}" -ge 3 ]] && break
  sleep 1
done
if [[ "${healthy}" -lt 3 ]]; then
  echo "  only ${healthy} of 3 nodes came up. Router log:" >&2
  tail -8 "${OUT}/router.log" >&2
  exit 4
fi

echo "  driving ${ROUNDS} rounds so the screens have something on them"
python3 bench/loadgen.py --router "http://${IP}:${PORT}" --token "${TOKEN}" \
    --rounds "${ROUNDS}" --subagents "${SUBAGENTS}" --label console 2>&1 \
    | grep -E 'WALL-CLOCK|cold starts|per node' || true

cat <<EOM

  console   http://${IP}:${PORT}/?token=${TOKEN}
  trace     ${OUT}/console.jsonl
  stop      bench/console.sh --stop
EOM
