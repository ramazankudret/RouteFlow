#!/usr/bin/env python3
"""RouteFlow trace reporter.

Reads one or more trace files (docs/TRACE-SCHEMA.md v1) and prints the numbers
the build phases are judged on. Phase 1's exit criterion is scenario wall-clock,
median of N runs, with p50/p95 latency reported alongside but not decisive
(D12) — so this tool reports wall-clock first and never collapses the two
prediction errors into one number (§8).

  python3 bench/trace_report.py trace.jsonl
  python3 bench/trace_report.py --detail run-a.jsonl run-b.jsonl

A malformed line is skipped and counted, never fatal (schema rule 4).
"""

import argparse
import json
import math
import sys
from collections import defaultdict


def load(path):
    """Returns (records, parse_errors). Rule 4: skip and count, never fatal."""
    records, errors = [], 0
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                errors += 1
                continue
            if not isinstance(record, dict) or record.get("v") is None:
                errors += 1
                continue
            records.append(record)
    return records, errors


def percentile(values, q):
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    pos = (len(ordered) - 1) * q
    lo, hi = math.floor(pos), math.ceil(pos)
    if lo == hi:
        return ordered[lo]
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (pos - lo)


def iso_ms(value):
    """Parses the schema's ISO-8601-with-milliseconds form to epoch ms."""
    if not value:
        return None
    import datetime

    try:
        text = value.replace("Z", "+00:00")
        return datetime.datetime.fromisoformat(text).timestamp() * 1000.0
    except ValueError:
        return None


def fmt(value, width=8, digits=0, unit=""):
    if value is None:
        return "—".rjust(width)
    return f"{value:,.{digits}f}{unit}".rjust(width)


def report(path, detail=False):
    records, parse_errors = load(path)
    print(f"\n=== {path} ===")
    if not records:
        print("  no usable records"
              + (f" ({parse_errors} malformed lines skipped)" if parse_errors else ""))
        return

    ok = [r for r in records if r.get("outcome") == "ok"]
    by_outcome = defaultdict(int)
    for r in records:
        by_outcome[r.get("outcome", "?")] += 1

    # --- the primary metric (D12) -------------------------------------------
    starts = [t for t in (iso_ms(r.get("ts_received")) for r in records) if t]
    ends = [t for t in (iso_ms(r.get("ts_done")) for r in records) if t]
    wall = (max(ends) - min(starts)) / 1000.0 if starts and ends else None

    policies = sorted({r.get("policy", "?") for r in records})
    models = sorted({r.get("cost_model", "?") for r in records})
    print(f"  policy      {', '.join(policies)}")
    print(f"  cost model  {', '.join(models)}")
    print(f"  records     {len(records)}  ("
          + ", ".join(f"{k}={v}" for k, v in sorted(by_outcome.items()))
          + (f", {parse_errors} malformed" if parse_errors else "") + ")")
    print(f"  WALL-CLOCK  {fmt(wall, 8, 2)} s   <- Phase 1 decides on this (D12)")

    # --- latency, reported but not decisive ---------------------------------
    totals = [r["total_ms"] for r in ok if r.get("total_ms") is not None]
    # TTFT is null on buffered replies by design: a first byte that is also the
    # whole answer is not a time-to-first-token.
    ttfts = [r["ttft_ms"] for r in ok if r.get("ttft_ms") is not None]
    print(f"  total  p50  {fmt(percentile(totals, 0.5))} ms   "
          f"p95 {fmt(percentile(totals, 0.95))} ms")
    if ttfts:
        print(f"  ttft   p50  {fmt(percentile(ttfts, 0.5))} ms   "
              f"p95 {fmt(percentile(ttfts, 0.95))} ms   "
              f"({len(ttfts)}/{len(ok)} streamed)")
    else:
        print("  ttft        —          (no streamed requests)")

    # --- warmth, the thing the project is about ------------------------------
    cold = [r for r in ok if not r.get("was_resident")]
    warm = [r for r in ok if r.get("was_resident")]
    print(f"  cold starts {len(cold)}/{len(ok)}"
          + (f"   warm p50 {fmt(percentile([r['total_ms'] for r in warm], 0.5), 6)} ms"
             f"   cold p50 {fmt(percentile([r['total_ms'] for r in cold], 0.5), 6)} ms"
             if warm and cold else ""))
    measured_load = [r["load_ms"] for r in ok
                     if r.get("load_ms") is not None and r["load_ms"] > 0]
    unmeasured = [r for r in cold if r.get("load_ms") is None]
    if measured_load:
        print(f"  load p50    {fmt(percentile(measured_load, 0.5))} ms "
              f"({len(measured_load)} measured)")
    if unmeasured:
        print(f"  load        {len(unmeasured)} cold start(s) with no engine-reported "
              f"load time (null, not zero)")

    # --- the two prediction errors, kept apart (§8) --------------------------
    timing = [(r["predicted_total_ms"], r["total_ms"]) for r in ok
              if r.get("predicted_total_ms") and r.get("total_ms")]
    if timing:
        rel = [abs(p - a) / a for p, a in timing if a > 0]
        print(f"  timing err  {fmt(percentile(rel, 0.5) * 100, 8, 1)} %  median "
              f"|predicted-actual|/actual")
    length = [(r["predicted_output_tokens"], r["output_tokens"]) for r in ok
              if r.get("predicted_output_tokens") and r.get("output_tokens")]
    if length:
        rel = [abs(p - a) / a for p, a in length if a > 0]
        print(f"  length err  {fmt(percentile(rel, 0.5) * 100, 8, 1)} %  median "
              f"output-token error (tracked separately, §8)")
    prompt = [(r["prompt_tokens_est"], r["prompt_tokens_actual"]) for r in records
              if r.get("prompt_tokens_actual")]
    if prompt:
        rel = [(e - a) / a for e, a in prompt if a > 0]
        print(f"  prompt err  {fmt(percentile(rel, 0.5) * 100, 8, 1)} %  median signed "
              f"(D15: estimated, never tokenized)")

    # --- routing -------------------------------------------------------------
    by_node = defaultdict(int)
    by_decided = defaultdict(int)
    for r in ok:
        by_node[r.get("node_id", "?")] += 1
        by_decided[r.get("decided_by", "?")] += 1
    print("  nodes       " + ", ".join(f"{k}={v}" for k, v in sorted(by_node.items())))
    print("  decided by  " + ", ".join(f"{k}={v}"
                                       for k, v in sorted(by_decided.items())))
    noise = by_decided.get("within_noise", 0)
    if noise:
        print(f"              {noise} decision(s) were inside the uncertainty band")

    rejects = defaultdict(int)
    for r in records:
        for c in r.get("candidates", []):
            if not c.get("admitted"):
                rejects[c.get("reason", "?")] += 1
    if rejects:
        print("  rejections  " + ", ".join(f"{k}={v}"
                                           for k, v in sorted(rejects.items())))

    if detail:
        print()
        header = (f"  {'job':>6}  {'node':<12} {'warm':<5} {'strm':<5} "
                  f"{'tok est/act':>12} {'ttft':>7} {'total':>7} {'load':>8} "
                  f"{'pred':>8}  outcome")
        print(header)
        print("  " + "-" * (len(header) - 2))
        for r in records:
            est = r.get("prompt_tokens_est")
            act = r.get("prompt_tokens_actual")
            print(f"  {r.get('job_id', '')[-5:]:>6}  "
                  f"{r.get('node_id', '—'):<12} "
                  f"{'yes' if r.get('was_resident') else 'no':<5} "
                  f"{'yes' if r.get('stream') else 'no':<5} "
                  f"{f'{est}/{act}' if act else f'{est}/—':>12} "
                  f"{fmt(r.get('ttft_ms'), 7)} "
                  f"{fmt(r.get('total_ms'), 7)} "
                  f"{fmt(r.get('load_ms'), 8)} "
                  f"{fmt(r.get('predicted_total_ms'), 8)}  "
                  f"{r.get('outcome')}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("traces", nargs="+", help="trace .jsonl files")
    parser.add_argument("--detail", action="store_true", help="one line per record")
    args = parser.parse_args()
    for path in args.traces:
        try:
            report(path, args.detail)
        except OSError as exc:
            print(f"\n=== {path} ===\n  {exc}", file=sys.stderr)
    print()


if __name__ == "__main__":
    main()
