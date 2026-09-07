#!/usr/bin/env python3
"""RouteFlow — aggregate a policy comparison across runs (ARCHITECTURE §9, D12).

Reads every trace a comparison produced and puts the two policies side by side.
Wall-clock is the metric Phase 1 is decided on; everything else is reported
because it can move in the opposite direction and that is worth seeing, not
because it settles anything.

The last block is the one that keeps the result honest. A wall-clock win does
not by itself say the win came from *warmth*: a policy that also declines to use
a slow node gets some of its advantage from node speed, which any speed-aware
policy would have found. Replaying each decision without the T_load term
separates the two, and it needs no extra runs because every record carries the
full per-candidate breakdown (D10).

  python3 bench/summarize.py bench/results
"""

import argparse
import datetime
import glob
import json
import os
import statistics
import sys
from collections import defaultdict


def iso_ms(value):
    if not value:
        return None
    try:
        return datetime.datetime.fromisoformat(
            value.replace("Z", "+00:00")).timestamp() * 1000.0
    except ValueError:
        return None


def percentile(values, q):
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    pos = (len(ordered) - 1) * q
    lo, hi = int(pos), min(int(pos) + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (pos - lo)


def load_records(path):
    out = []
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue  # rule 4: skip and carry on
            if isinstance(record, dict) and record.get("v") is not None:
                out.append(record)
    return out


def speed_only_pick(candidates):
    """Which node a policy that ignored warmth entirely would have chosen.
    Every other term is kept, so the only difference is T_load."""
    admitted = [c for c in candidates if c.get("admitted")]
    if len(admitted) < 2:
        return None
    def score(c):
        return sum((c.get(k) or 0)
                   for k in ("t_queue", "t_prefill", "t_decode", "t_evict"))
    return min(admitted, key=score).get("node_id")


def aggregate(paths):
    agg = {
        "runs": 0, "walls": [], "ttft": [], "total": [], "cold": [],
        "timing": [], "length": [], "decided": defaultdict(int),
        "flips": 0, "comparable": 0, "load_spread": [],
        "covered": 0, "banded": 0,
    }
    for path in paths:
        records = load_records(path)
        ok = [r for r in records if r.get("outcome") == "ok"]
        if not ok:
            continue
        agg["runs"] += 1

        starts = [t for t in (iso_ms(r.get("ts_received")) for r in ok) if t]
        ends = [t for t in (iso_ms(r.get("ts_done")) for r in ok) if t]
        if starts and ends:
            agg["walls"].append((max(ends) - min(starts)) / 1000.0)

        agg["ttft"] += [r["ttft_ms"] for r in ok if r.get("ttft_ms") is not None]
        agg["total"] += [r["total_ms"] for r in ok if r.get("total_ms") is not None]
        agg["cold"].append(sum(1 for r in ok if not r.get("was_resident")))
        agg["timing"] += [abs(r["predicted_total_ms"] - r["total_ms"]) / r["total_ms"]
                          for r in ok
                          if r.get("predicted_total_ms") and r.get("total_ms", 0) > 0]
        # Length error is never folded into timing error (§8): they break for
        # different reasons and one would mask the other.
        agg["length"] += [abs(r["predicted_output_tokens"] - r["output_tokens"])
                          / r["output_tokens"]
                          for r in ok
                          if r.get("predicted_output_tokens")
                          and r.get("output_tokens", 0) > 0]
        # Does the uncertainty band mean anything? A sigma nobody checks is
        # decoration, and §6.2 spends it on real decisions — within_noise falls
        # back to the cheaper node whenever two estimates overlap. If coverage
        # is far from the ~68% a 1-sigma band claims, that fallback is either
        # firing on differences that were real or refusing to fire on noise.
        for r in ok:
            sigma = r.get("predicted_sigma_ms")
            if not sigma or not r.get("predicted_total_ms") or not r.get("total_ms"):
                continue
            agg["banded"] += 1
            if abs(r["predicted_total_ms"] - r["total_ms"]) <= sigma:
                agg["covered"] += 1

        for r in ok:
            agg["decided"][r.get("decided_by", "?")] += 1
            candidates = r.get("candidates", [])
            pick = speed_only_pick(candidates)
            if pick is None:
                continue
            agg["comparable"] += 1
            if pick != r.get("node_id"):
                agg["flips"] += 1
            loads = [(c.get("t_load") or 0) for c in candidates if c.get("admitted")]
            if len(loads) >= 2:
                agg["load_spread"].append(max(loads) - min(loads))
    return agg


def fmt(value, digits=0):
    return "—" if value is None else f"{value:,.{digits}f}"


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("results_dir", nargs="?", default="bench/results")
    parser.add_argument("--baseline", default="roundrobin-v1")
    parser.add_argument("--policy", default="warmth-v1")
    args = parser.parse_args()

    groups = {}
    for name in (args.baseline, args.policy):
        paths = sorted(glob.glob(os.path.join(args.results_dir, f"{name}-run*.jsonl")))
        if not paths:
            print(f"no traces for '{name}' in {args.results_dir}", file=sys.stderr)
            return 2
        groups[name] = aggregate(paths)

    base, test = groups[args.baseline], groups[args.policy]

    def row(label, a, b, digits=0, lower_is_better=True):
        if a is None or b is None:
            print(f"  {label:<26}{fmt(a, digits):>14}{fmt(b, digits):>14}")
            return
        delta = (a - b) / a * 100.0 if a else 0.0
        mark = ""
        if abs(delta) >= 1:
            improved = delta > 0 if lower_is_better else delta < 0
            mark = "  better" if improved else "  worse"
        print(f"  {label:<26}{fmt(a, digits):>14}{fmt(b, digits):>14}"
              f"{delta:>+9.1f}%{mark}")

    # Unequal arms mean a run failed or a trace is missing, and a median over
    # whatever survived is a comparison between two different experiments.
    if base["runs"] != test["runs"]:
        print(f"\nREFUSING TO COMPARE: {args.baseline} has {base['runs']} run(s), "
              f"{args.policy} has {test['runs']}.", file=sys.stderr)
        print("A missing run is a failed run. Fix it and re-measure rather than "
              "averaging over what is left.", file=sys.stderr)
        return 3

    print(f"\n{'':<28}{args.baseline:>14}{args.policy:>14}")
    print("  " + "-" * 62)
    print("  PRIMARY — wall-clock (D12)")
    row("wall-clock median (s)", percentile(base["walls"], 0.5),
        percentile(test["walls"], 0.5), 2)
    base_spread = f"{min(base['walls']):.1f}-{max(base['walls']):.1f}"
    test_spread = f"{min(test['walls']):.1f}-{max(test['walls']):.1f}"
    label = "  spread across runs"
    print(f"  {label:<26}{base_spread:>14}{test_spread:>14}")
    print(f"  {'  runs':<26}{base['runs']:>14}{test['runs']:>14}")

    # Min-max ranges are the wrong summary for two small samples: a single
    # touching pair reads as "overlapping" even when every other comparison
    # goes one way. Counting the pairings is what the medians are actually
    # claiming, and it is the statistic a rank test is built on.
    pairs = [(a, b) for a in test['walls'] for b in base['walls']]
    wins = sum(1 for a, b in pairs if a < b)
    if pairs:
        n = len(pairs)
        print(f"  {'  pairwise wins':<26}{'':>14}{f'{wins}/{n}':>14}")
        # Exact Mann-Whitney thresholds for equal group sizes, two-tailed 0.05.
        critical = {4: 15, 5: 23, 6: 32, 7: 42, 8: 54}
        k = min(base['runs'], test['runs'])
        if k in critical and base['runs'] == test['runs']:
            verdict = ("outside run-to-run variance (p < 0.05)"
                       if wins >= critical[k] or wins <= n - critical[k]
                       else "NOT distinguishable from run-to-run variance")
            print(f"  {'':<26}{'':>14}{'':>14}  {verdict}")

    print("\n  REPORTED — can move against wall-clock; not decisive")
    row("total p50 (ms)", percentile(base["total"], 0.5), percentile(test["total"], 0.5))
    row("total p95 (ms)", percentile(base["total"], 0.95),
        percentile(test["total"], 0.95))
    row("ttft p50 (ms)", percentile(base["ttft"], 0.5), percentile(test["ttft"], 0.5))
    row("ttft p95 (ms)", percentile(base["ttft"], 0.95), percentile(test["ttft"], 0.95))
    # Phase 3 is decided on this pair, not on wall-clock: cold starts must fall
    # and p95 must not regress (§9). Both are already printed above; the label
    # here is a reminder of which line is the criterion for which phase.
    row("cold starts per run", statistics.median(base["cold"]),
        statistics.median(test["cold"]))
    row("timing error (%)", percentile(base["timing"], 0.5) * 100,
        percentile(test["timing"], 0.5) * 100, 1)
    if base["length"] or test["length"]:
        row("length error (%)",
            percentile(base["length"], 0.5) * 100 if base["length"] else None,
            percentile(test["length"], 0.5) * 100 if test["length"] else None, 1)
    if base["banded"] and test["banded"]:
        # Printed without a better/worse mark on purpose: neither direction is
        # good. A 1-sigma band should cover about 68%. Far below and the band is
        # too narrow to justify falling back on ties; far above and it is so
        # wide that within_noise swallows differences that were real.
        a = base["covered"] / base["banded"] * 100.0
        b = test["covered"] / test["banded"] * 100.0
        print(f"  {'within 1 sigma (%)':<26}{a:>14.0f}{b:>14.0f}"
              f"      (~68% is honest)")

    print("\n  ATTRIBUTION — how much of this is actually warmth?")
    for name, group in ((args.baseline, base), (args.policy, test)):
        if not group["comparable"]:
            continue
        pct = group["flips"] / group["comparable"] * 100.0
        print(f"    {name}: dropping t_load would change "
              f"{group['flips']}/{group['comparable']} decisions ({pct:.0f}%)")
    if test["load_spread"]:
        p50 = percentile(test["load_spread"], 0.5)
        p95 = percentile(test["load_spread"], 0.95)
        print(f"    t_load spread across candidates: p50 {p50:,.0f} ms, "
              f"p95 {p95:,.0f} ms")
        if p50 == 0:
            print("    (a p50 of zero means half the decisions had no warmth "
                  "difference to act on at all)")

    print(f"\n  {args.policy} decided_by: " +
          ", ".join(f"{k}={v}" for k, v in
                    sorted(test["decided"].items(), key=lambda kv: -kv[1])))
    noise = test["decided"].get("within_noise", 0)
    if noise:
        total = sum(test["decided"].values())
        print(f"    {noise}/{total} decisions were inside the uncertainty band — "
              f"the win rides on the rest (D8)")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
