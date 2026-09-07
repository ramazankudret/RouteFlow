#!/usr/bin/env bash
# Engine first, on loopback only, then the agent in front of it.
set -uo pipefail

: "${RF_NODE_ID:?RF_NODE_ID is required}"
: "${RF_TOKEN:?RF_TOKEN is required}"
RF_TELEMETRY="${RF_TELEMETRY:-null}"
RF_SLOTS="${RF_SLOTS:-1}"

# The engine binds loopback inside this container's netns, so nothing outside
# can reach it even if the container's port were published. The agent is the
# front door, and it authenticates.
OLLAMA_HOST=127.0.0.1:11434 /bin/ollama serve &

for _ in $(seq 1 60); do
  curl -s -m 2 http://127.0.0.1:11434/api/tags >/dev/null 2>&1 && break
  sleep 1
done

exec /rf/routeflow-agent \
  --node.id "${RF_NODE_ID}" \
  --http.bind 0.0.0.0 --http.port 8971 --http.token "${RF_TOKEN}" \
  --engine.kind ollama --engine.endpoint 127.0.0.1:11434 \
  --engine.slots "${RF_SLOTS}" \
  --telemetry "${RF_TELEMETRY}" \
  --log.level warn
