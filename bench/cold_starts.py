#!/usr/bin/env python3
"""Classify cold starts in a trace as first-touch or repeat.

The distinction decides whether reactive placement can help at all. A
first-touch cold start is the first time a model is asked for on a node — no
reactive strategy can prevent it, because demand is only known after a request
has already paid for it. A repeat is a model that was resident, got evicted, and
had to be loaded again; that is the only kind placement can remove.

A scenario whose cold starts are all first-touches is not a test of placement.

  python3 bench/cold_starts.py bench/results-phase3/lru-run1.jsonl
"""

import glob
import json
import sys


def classify(path):
    seen = set()
    first = repeat = warm = 0
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            continue
        if r.get("outcome") != "ok" or not r.get("node_id"):
            continue
        key = (r["node_id"], r["model"])
        if r.get("was_resident"):
            warm += 1
        elif key in seen:
            repeat += 1
        else:
            first += 1
        seen.add(key)
    return first, repeat, warm


def main():
    paths = []
    for arg in sys.argv[1:]:
        paths.extend(sorted(glob.glob(arg)))
    if not paths:
        print(__doc__)
        return 2

    totals = [0, 0, 0]
    for path in paths:
        first, repeat, warm = classify(path)
        totals = [totals[0] + first, totals[1] + repeat, totals[2] + warm]
        print(f"  {path.split('/')[-1]:<28} first-touch {first:>3}   "
              f"repeat {repeat:>3}   warm {warm:>3}")

    print(f"\n  {'total':<28} first-touch {totals[0]:>3}   "
          f"repeat {totals[1]:>3}   warm {totals[2]:>3}")
    if totals[1] == 0 and totals[0] > 0:
        print("\n  Every cold start is a first touch. Reactive placement cannot")
        print("  remove any of these — it learns what to preload from the very")
        print("  requests that paid for them. This scenario does not test it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
