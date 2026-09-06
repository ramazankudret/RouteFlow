#!/usr/bin/env bash
# End to end on real hardware: router -> agent -> Ollama -> RTX 4060.
#
# Phase 0's exit criterion was "a real request proxied through the router to
# Ollama, and a trace file whose every field is populated with a correct value".
# That was checked once, early, and everything comparative since has run on
# simulated nodes. This re-checks it against the code as it stands now, and adds
# the things that did not exist then: an uncapped output prediction, and the
# footprint ratio D35 learns from the engine's own report.
#
# Deliberately light on the card. qwen2.5:7b is 4.9 GB resident on an 8 GB GPU,
# outputs are short, and requests are serial.
set -uo pipefail
cd /mnt/c/Users/erama/OneDrive/Desktop/RouteFlow
BIN="$HOME/rf-build"
OUT=bench/results-real
MODEL="qwen2.5:7b-instruct-q4_K_M"
AGENT_PORT=8991
ROUTER_PORT=8990

mkdir -p "${OUT}"
pkill -f '[r]outeflow-(agent|router)' 2>/dev/null || true
sleep 1

cleanup() { pkill -f '[r]outeflow-(agent|router)' 2>/dev/null || true; }
trap cleanup EXIT

# Start from a genuinely cold card, or the first request never loads and the
# window this run exists to exercise never opens.
curl -s -m 60 "http://127.0.0.1:11434/api/generate" \
     -d "{\"model\":\"${MODEL}\",\"prompt\":\"\",\"keep_alive\":0}" >/dev/null 2>&1
sleep 3

echo "=== engine ==="
curl -s -m 5 http://127.0.0.1:11434/api/tags >/dev/null || { echo "ollama unreachable"; exit 1; }
nvidia-smi --query-gpu=name,memory.total,memory.used --format=csv,noheader | sed 's/^/  /'

echo
echo "=== agent against the real engine ==="
"${BIN}/routeflow-agent" --node.id rtx4060 --http.port "${AGENT_PORT}" \
    --engine.kind ollama --engine.endpoint 127.0.0.1:11434 \
    --log.level warn > "${OUT}/agent.log" 2>&1 &
sleep 3
curl -s -m 5 "http://127.0.0.1:${AGENT_PORT}/state" | python3 -c "
import json, sys
j = json.load(sys.stdin)
print('  node            ', j.get('id'))
print('  gpu             ', j.get('gpu_name'))
print('  telemetry       ', j.get('telemetry_backend'), 'ok' if j.get('telemetry_ok') else 'UNAVAILABLE')
print('  vram            %.2f GB total, %.2f GB free' % (j.get('vram_total_bytes',0)/1e9,
                                                         j.get('vram_free_bytes',0)/1e9))
print('  engine          ', j.get('engine'), 'healthy' if j.get('engine_healthy') else 'DOWN',
      '| slots', j.get('engine_slots'))
print('  residency known ', j.get('residency_known'))
print('  models on disk  ', len(j.get('models_on_disk') or []))
print('  resident        ', json.dumps(j.get('models_resident')))
"

cat > "${OUT}/nodes.json" <<JSON
{ "nodes": [ { "id": "rtx4060", "endpoint": "127.0.0.1:${AGENT_PORT}" } ] }
JSON

echo
echo "=== router ==="
rm -f "${OUT}/real.jsonl"
"${BIN}/routeflow-router" --nodes "${OUT}/nodes.json" --trace "${OUT}/real.jsonl" \
    --http.port "${ROUTER_PORT}" --policy warmth-v1 --cost_model learned-v1 \
    --replay_trace false --log.level warn > "${OUT}/router.log" 2>&1 &
sleep 3

ask() {  # $1 label, $2 prompt, $3 extra json
  echo "  -> $1"
  curl -s -m 300 "http://127.0.0.1:${ROUTER_PORT}/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -H 'X-RouteFlow-Role: subagent' \
    -d "{\"model\":\"${MODEL}\",\"messages\":[{\"role\":\"user\",\"content\":\"$2\"}],\"stream\":true$3}" \
    | tail -c 120
  echo
}

echo "requests (streaming, so ttft is a real measurement):"
# First one is cold: the model is not loaded yet, so T_load is exercised.
ask "cold, capped"   "Say hello in one short sentence." ",\"max_tokens\":24"
ask "warm, capped"   "Name three colours, comma separated." ",\"max_tokens\":24"
ask "warm, uncapped" "Reply with exactly the word: ok" ""
ask "warm, uncapped" "Count from one to five." ""

sleep 2
echo
echo "=== what the router learned about the footprint ==="
curl -s -m 5 "http://127.0.0.1:${ROUTER_PORT}/api/nodes" | python3 -c "
import json, sys
n = json.load(sys.stdin)[0]
for m in n.get('models_resident', []):
    disk = dict(n.get('model_disk_bytes') or {}).get(m['name'], 0)
    if disk:
        print('  %s: disk %.3f GB, resident %.3f GB, ratio %.4f'
              % (m['name'], disk/1e9, m['vram_bytes']/1e9, m['vram_bytes']/disk))
    else:
        print('  %s: resident %.3f GB (no disk size reported)' % (m['name'], m['vram_bytes']/1e9))
"

echo
echo "=== trace ==="
python3 bench/trace_report.py "${OUT}/real.jsonl"
