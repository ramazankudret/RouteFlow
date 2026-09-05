#!/usr/bin/env bash
# D22 states three properties of GET /snapshot that nothing was pinning.
#
# They are security and contract properties, not behaviour a unit test can
# reach: the handler lives inline in router/main.cpp behind a real socket, so
# this exercises the built binary rather than a linked function. That is the
# whole reason it is a script and not part of rf_selftest.
#
#   1. The same bearer token as every other read endpoint. No loopback
#      exemption, no separate key. A token-less request is 401.
#   2. The collector identity is fixed. `id` is "routeflow" and `kind` is
#      "inference" -- an identity, not a setting, so neither is configurable
#      and neither may drift.
#   3. The control surface gains no shortcut. /admin/* and the inference
#      endpoints stay behind the token and stay out of the envelope.
#
#   tests/snapshot_auth.sh [--bin ~/rf-build]

set -uo pipefail

BIN="${HOME}/rf-build"
PORT=8979
while [[ $# -gt 0 ]]; do
  case "$1" in
    --bin)  BIN="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
TOKEN="selftest-$$"
FAILURES=0

cleanup() {
  # An EXIT trap whose last command succeeds will hand that success back as the
  # script's status, which would report a failing run as a passing one.
  local rc=$?
  [[ -n "${ROUTER_PID:-}" ]] && kill "${ROUTER_PID}" 2>/dev/null
  [[ -n "${AGENT_PID:-}" ]] && kill "${AGENT_PID}" 2>/dev/null
  rm -rf "${WORK}"
  return ${rc}
}
trap cleanup EXIT

ok()   { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; FAILURES=$((FAILURES + 1)); }

check_eq() {
  local what="$1" want="$2" got="$3"
  if [[ "${want}" == "${got}" ]]; then ok "${what}"; else
    fail "${what}: expected '${want}', got '${got}'"
  fi
}

status_of() {  # $1 path, $2 optional bearer
  local args=(-s -o /dev/null -w '%{http_code}' -m 5)
  [[ -n "${2:-}" ]] && args+=(-H "Authorization: Bearer $2")
  curl "${args[@]}" "http://127.0.0.1:${PORT}$1"
}

echo "{ \"nodes\": [ { \"id\": \"sim\", \"endpoint\": \"127.0.0.1:8978\" } ] }" \
  > "${WORK}/nodes.json"

"${BIN}/routeflow-agent" --simulate "${REPO}/bench/profiles/sim-desktop.json" \
    --http.port 8978 --node.id sim --log.level error >/dev/null 2>&1 &
AGENT_PID=$!
sleep 1

"${BIN}/routeflow-router" --config "${REPO}/bench/router.json" \
    --nodes "${WORK}/nodes.json" --trace "${WORK}/t.jsonl" \
    --http.port "${PORT}" --http.token "${TOKEN}" \
    --log.level error >/dev/null 2>&1 &
ROUTER_PID=$!

for _ in $(seq 1 20); do
  [[ "$(status_of /health)" == "200" ]] && break
  sleep 0.5
done

echo "snapshot auth (D22)"
check_eq "no token is refused"      401 "$(status_of /snapshot)"
check_eq "a wrong token is refused" 401 "$(status_of /snapshot wrong-token)"
check_eq "the shared token is accepted" 200 "$(status_of /snapshot "${TOKEN}")"
check_eq "/health stays open"       200 "$(status_of /health)"

echo "control surface (D22)"
check_eq "/admin/stats needs the token"  401 "$(status_of /admin/stats)"
check_eq "/admin/policy needs the token" 401 "$(status_of /admin/policy)"

echo "envelope contract (D22)"
BODY="${WORK}/snapshot.json"
curl -s -m 5 -H "Authorization: Bearer ${TOKEN}" \
     "http://127.0.0.1:${PORT}/snapshot" > "${BODY}"

# Every check below reads this body. If it is not JSON there is one thing wrong,
# not eight, and a page of interpreter tracebacks would bury it.
if ! python3 -c "import json,sys; json.load(open('${BODY}'))" 2>/dev/null; then
  fail "the envelope did not come back as JSON ($(wc -c < "${BODY}") bytes)"
  echo
  echo "snapshot_auth: ${FAILURES} failure(s)"
  exit 1
fi

read_field() { python3 -c "
import json, sys
j = json.load(open(sys.argv[1]))
cur = j
for part in sys.argv[2].split('.'):
    cur = cur.get(part) if isinstance(cur, dict) else None
    if cur is None: break
print('' if cur is None else cur)
" "${BODY}" "$1"; }

check_eq "collector.id is fixed"   routeflow "$(read_field collector.id)"
check_eq "collector.kind is fixed" inference "$(read_field collector.kind)"
check_eq "envelope version"        1         "$(read_field v)"

# now_ns is an epoch in nanoseconds, ~1.8e18, past the 2^53 ceiling for exact
# integers in a double. D22 records that Json used to serialise it as
# 1.78854201051094e+18; a consumer reading that back gets a different instant.
python3 - "${BODY}" <<'PY'
import json, re, sys
raw = open(sys.argv[1]).read()
value = json.loads(raw).get("now_ns")
# Match the literal as it was written, not as json.loads rebuilt it: a float
# round-trips back to a Python number and the damage would not show. The
# scan has to stop at the number, since "nodes" a few bytes later contains
# an 'e' and would look like an exponent.
literal = re.search(r'"now_ns"\s*:\s*([^,}\s]+)', raw)
text = literal.group(1) if literal else ''
bad = (not isinstance(value, int)) or ('e' in text.lower()) or ('.' in text)
print(("  FAIL  now_ns survives as an exact integer: got %s" % (text or 'nothing',))
      if bad else "  ok    now_ns survives as an exact integer")
sys.exit(1 if bad else 0)
PY
if [[ $? -ne 0 ]]; then FAILURES=$((FAILURES + 1)); fi

# The body sits at the same level as the envelope, and its field names are the
# ones the UI and NoteFlow already read. A rename here is a breaking change for
# both, so the names are pinned rather than trusted.
for field in nodes jobs_recent models_warm policy cost_model; do
  if python3 -c "
import json, sys
sys.exit(0 if '${field}' in json.load(open('${BODY}')) else 1)"; then
    ok "body carries ${field}"
  else
    fail "body is missing ${field}"
  fi
done

echo
if [[ ${FAILURES} -eq 0 ]]; then
  echo "snapshot_auth: all checks passed"
else
  echo "snapshot_auth: ${FAILURES} failure(s)"
fi
exit $(( FAILURES > 0 ? 1 : 0 ))
