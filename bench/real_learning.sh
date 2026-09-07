#!/usr/bin/env bash
# Does the cost model actually learn from a real engine?
#
# Every campaign in docs/ was measured against simulated nodes, which always
# report their token counts. A real engine on the OpenAI path does not, unless
# asked (D37) -- and until it was asked, the learned model had nothing to learn
# from and quietly behaved like the static one. This checks that the fix works
# where it matters, on hardware.
#
# One GPU means one candidate, so there is no routing decision and wall-clock
# says nothing. What is measurable is prediction error: the same real traffic,
# scored first by the seeded model and then by one that has read a trace of it.
#
# The D25 protocol, unchanged: the learned arm replays a warm-up written by an
# earlier run and never learns from the run being measured on itself.
#
#   bench/real_learning.sh [--requests 30] [--model NAME]
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

BIN="${HOME}/rf-build"
OUT=bench/results-real-learning
MODEL="qwen2.5:7b-instruct-q4_K_M"
REQUESTS=30
OLLAMA=127.0.0.1:11434
AGENT_PORT=8991
ROUTER_PORT=8990

while [[ $# -gt 0 ]]; do
  case "$1" in
    --requests) REQUESTS="$2"; shift 2 ;;
    --model)    MODEL="$2"; shift 2 ;;
    --bin)      BIN="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

mkdir -p "${OUT}"
cleanup() { pkill -f '[r]outeflow-(agent|router)' 2>/dev/null || true; }
# An EXIT trap that returns normally hands bash the trap's status, not the
# script's, so every `exit N` below was reported to the caller as 0 -- the
# refusal printed and the harness looked like it had passed. Re-exiting with
# the saved status is the fix.
trap 'rc=$?; cleanup; exit ${rc}' EXIT
cleanup; sleep 1

curl -s -m 5 "http://${OLLAMA}/api/tags" >/dev/null || { echo "ollama unreachable"; exit 1; }

echo '{ "nodes": [ { "id": "rtx4060", "endpoint": "127.0.0.1:'"${AGENT_PORT}"'" } ] }' \
  > "${OUT}/nodes.json"

# One driver for all three arms, so the three see identical traffic. Sizes come
# from a fixed seed: same run, same prompts, same caps (D12).
cat > "${OUT}/drive.py" <<'PY'
import json, random, sys, urllib.request

router, count, model, seed = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
rng = random.Random(seed)
words = ["context", "state", "plan", "step", "tool", "result", "note", "value"]
ok = failed = 0
for i in range(count):
    prompt_tokens = rng.randint(80, 400)
    out_tokens = rng.randint(24, 96)
    body = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content":
                      " ".join(rng.choice(words) for _ in range(int(prompt_tokens * 0.75)))}],
        "max_tokens": out_tokens,
        "stream": True,
    }).encode()
    req = urllib.request.Request(router + "/v1/chat/completions", data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("X-RouteFlow-Role", "subagent")
    try:
        with urllib.request.urlopen(req, timeout=600) as r:
            r.read()
        ok += 1
    except Exception as exc:      # a failed request is data, not a crash
        failed += 1
        print("  request %d failed: %s" % (i, exc), file=sys.stderr)
    if (i + 1) % 10 == 0:
        print("  %d/%d" % (i + 1, count), file=sys.stderr)
print("ok %d, failed %d" % (ok, failed), file=sys.stderr)
PY

start_cluster() {   # $1 cost model, $2 trace, $3 replay ("" for none)
  cleanup; sleep 1
  "${BIN}/routeflow-agent" --node.id rtx4060 --http.port "${AGENT_PORT}" \
      --engine.kind ollama --engine.endpoint "${OLLAMA}" \
      --log.level error >/dev/null 2>&1 &
  sleep 3
  local replay=(--replay_trace false)
  [[ -n "$3" ]] && replay=(--replay_from "$3")
  rm -f "$2"
  "${BIN}/routeflow-router" --nodes "${OUT}/nodes.json" --trace "$2" \
      --http.port "${ROUTER_PORT}" --policy warmth-v1 --cost_model "$1" \
      "${replay[@]}" --log.level warn > "${OUT}/router-$1.log" 2>&1 &
  sleep 3
}

run_arm() {         # $1 label, $2 cost model, $3 trace, $4 replay
  echo "=== $1 ==="
  start_cluster "$2" "$3" "$4"
  python3 "${OUT}/drive.py" "http://127.0.0.1:${ROUTER_PORT}" "${REQUESTS}" "${MODEL}" 11
  echo
}

# The warm-up exists to be read, not measured. Its own cold start is in there,
# which is the point: the learned arm should see one.
run_arm "warm-up (static, ${REQUESTS} requests, written for the learned arm to read)" \
        static-v1 "${OUT}/warmup.jsonl" ""
run_arm "baseline (static, seeded)" static-v1 "${OUT}/static.jsonl" ""
run_arm "learned (replaying the warm-up)" learned-v1 "${OUT}/learned.jsonl" "${OUT}/warmup.jsonl"

cleanup
echo
python3 - "${OUT}" <<'PY'
import json, io, os, sys

out = sys.argv[1]

def load(name):
    rows = []
    with io.open(os.path.join(out, name), encoding="utf-8") as fh:
        for line in fh:
            if line.strip():
                rows.append(json.loads(line))
    return [r for r in rows if r.get("outcome") == "ok"]

def median(v):
    if not v: return None
    s = sorted(v); m = len(s) // 2
    return s[m] if len(s) % 2 else (s[m-1] + s[m]) / 2

def err(rows):
    return [abs(r["predicted_total_ms"] - r["total_ms"]) / r["total_ms"] * 100
            for r in rows if r.get("predicted_total_ms") and r.get("total_ms")]

def counts(rows):
    return (sum(1 for r in rows if r.get("output_tokens")),
            sum(1 for r in rows if r.get("prompt_tokens_actual")),
            len(rows))

for name in ("warmup", "static", "learned"):
    rows = load(name + ".jsonl")
    o, p, n = counts(rows)
    print("%-8s %3d ok   output_tokens %d/%d   prompt_tokens_actual %d/%d"
          % (name, n, o, n, p, n))

print()
s, l = err(load("static.jsonl")), err(load("learned.jsonl"))
print("median |predicted - actual| / actual")
print("  static-v1   %6.1f %%   (n %d)" % (median(s), len(s)))
print("  learned-v1  %6.1f %%   (n %d)" % (median(l), len(l)))
if median(s):
    print("  change      %+6.1f %%" % ((median(l) - median(s)) / median(s) * 100))
print()
print("If the learned arm is not better, the fix did not take: with no counts to")
print("read it falls back to the same seeds the static model uses, and the two")
print("numbers come out equal.")
PY
