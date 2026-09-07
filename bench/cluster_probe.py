# -*- coding: utf-8 -*-
"""Fire N requests at the router and report exactly what came back.

Used by the fault tests, where "it worked" is not good enough: a request that
returns 200 with an empty body, or hangs until the client gives up, is a
different failure from a clean 503 and the harness has to tell them apart.
"""
import json, sys, time, urllib.request, urllib.error

router, token, n, model = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
# A short reply is over before a fault can land on it. Fault tests pass a
# large budget so the request is certainly still open when the node dies,
# which is the only way to reach §6.4's retry path.
max_tokens = int(sys.argv[5]) if len(sys.argv) > 5 else 24
# max_tokens is a ceiling, not a target: ask for one short sentence and the
# model stops after ten tokens however large the budget. A fault aimed at a
# request in flight needs a prompt that actually keeps it in flight, so the
# caller passes one.
prompt = sys.argv[6] if len(sys.argv) > 6 else "reply with one short sentence"
# Time-to-first-token only exists for a stream: on a buffered reply the first
# byte is the whole answer, so the router records ttft as null and any ttft
# analysis over those traces has nothing to check its estimates against.
stream = len(sys.argv) > 7 and sys.argv[7] == "stream"
ok = 0
codes = {}
slowest = 0.0
for i in range(n):
    body = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": max_tokens,
        "stream": stream,
    }).encode()
    req = urllib.request.Request(router + "/v1/chat/completions", data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Authorization", "Bearer " + token)
    t0 = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=180) as r:
            raw = r.read()
        if stream:
            # SSE: any chunk carrying content is proof the reply arrived.
            content = "".join(
                json.loads(line[6:]).get("choices", [{}])[0]
                    .get("delta", {}).get("content", "")
                for line in raw.decode("utf-8", "replace").splitlines()
                if line.startswith("data: ") and line[6:].strip() != "[DONE]")
        else:
            d = json.loads(raw)
            content = d.get("choices", [{}])[0].get("message", {}).get("content", "")
        if content:
            ok += 1
            codes["200"] = codes.get("200", 0) + 1
        else:
            codes["200-empty"] = codes.get("200-empty", 0) + 1
    except urllib.error.HTTPError as e:
        codes[str(e.code)] = codes.get(str(e.code), 0) + 1
    except Exception as e:
        codes[type(e).__name__] = codes.get(type(e).__name__, 0) + 1
    slowest = max(slowest, time.monotonic() - t0)

print("PROBE ok=%d of %d slowest=%.1fs codes=%s"
      % (ok, n, slowest, ",".join("%s:%d" % kv for kv in sorted(codes.items()))))
