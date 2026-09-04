#!/usr/bin/env python3
"""Print what the placement manager did. A preload is not a job and leaves no
trace record of its own, so this is the only place its work is visible."""
import json
import sys
import urllib.request

url = (sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8970") + "/admin/stats"
try:
    with urllib.request.urlopen(url, timeout=5) as r:
        p = json.load(r).get("placement", {})
except Exception as exc:  # noqa: BLE001
    print(f"  placement: unavailable ({exc})")
    sys.exit(0)

if not p.get("enabled"):
    print("  placement: off")
    sys.exit(0)

print(f"  placement: {p.get('preloads', 0)} preloads, {p.get('evictions', 0)} evictions, "
      f"{p.get('failures', 0)} failures, {p.get('skipped_busy', 0)} cycles skipped (busy)")
if p.get("last_action"):
    print(f"             last: {p['last_action']}")
demand = p.get("demand", {})
if demand:
    top = sorted(demand.values(), key=lambda d: -d.get("count", 0))[:5]
    print("             demand: " +
          ", ".join(f"{d['node']}/{d['model']}={d['count']}" for d in top))
