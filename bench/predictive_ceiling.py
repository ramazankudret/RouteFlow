#!/usr/bin/env python3
"""What is the most predictive placement could ever be worth here?

Phase 4 is predictive placement: learn which model tends to follow which from
the trace, and preload before the request arrives. ARCHITECTURE §9 defers it and
declines to commit to it, and D17 says to reconsider it with Phase 3's trace in
hand. This is that reconsideration, and it runs before any of it is built.

Two questions, both answerable from traces already collected.

**Is there structure to learn?** A first-order Markov predictor is scored
against the base rate -- always guessing the most common model. If knowing the
previous model does not beat knowing nothing, there is nothing to learn.

**Could a perfect predictor act in time?** This is the one that decides. A
preload only helps if it finishes before the request it was issued for arrives,
and a load takes seconds. Every assumption below is set in predictive
placement's favour, so the number it produces is a ceiling and not an estimate:

  - The predictor is an oracle. It knows the next model exactly, and the node
    the request will land on, which a real one would also have to predict.
  - It acts on the *arrival* of the previous request, the earliest a one-step
    predictor could possibly fire.
  - It is charged only for time the target node was idle, since a load issued
    into a busy engine queues behind the work in front of it. Idle time is
    summed across gaps, which flatters it further: a real load does not pause
    and resume around other jobs.

If a cold start still cannot be hidden under those terms, it cannot be hidden.

  python3 bench/predictive_ceiling.py "bench/results-phase2-uncapped/learned-v1-run*.jsonl"
"""

import argparse
import datetime
import glob
import json
import math
import sys
from collections import Counter, defaultdict


def iso_ms(value):
    if not value:
        return None
    try:
        return datetime.datetime.fromisoformat(
            value.replace("Z", "+00:00")).timestamp() * 1000.0
    except (ValueError, AttributeError):
        return None


def load(path):
    records = []
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            if r.get("outcome") != "ok":
                continue
            r["_recv"] = iso_ms(r.get("ts_received"))
            r["_disp"] = iso_ms(r.get("ts_dispatched"))
            r["_done"] = iso_ms(r.get("ts_done"))
            if None in (r["_recv"], r["_disp"], r["_done"]):
                continue
            records.append(r)
    records.sort(key=lambda r: r["_recv"])
    return records


def median(values):
    if not values:
        return None
    s = sorted(values)
    mid = len(s) // 2
    return s[mid] if len(s) % 2 else (s[mid - 1] + s[mid]) / 2.0


def merge(intervals):
    """Union of [start, end) spans, sorted."""
    out = []
    for start, end in sorted(intervals):
        if out and start <= out[-1][1]:
            out[-1][1] = max(out[-1][1], end)
        else:
            out.append([start, end])
    return out


def idle_between(busy, lo, hi):
    """(total idle ms, longest contiguous idle ms) in [lo, hi) given busy spans."""
    if hi <= lo:
        return 0.0, 0.0
    total, longest, cursor = 0.0, 0.0, lo
    for start, end in busy:
        if end <= lo or start >= hi:
            continue
        start, end = max(start, lo), min(end, hi)
        if start > cursor:
            gap = start - cursor
            total += gap
            longest = max(longest, gap)
        cursor = max(cursor, end)
    if cursor < hi:
        gap = hi - cursor
        total += gap
        longest = max(longest, gap)
    return total, longest


def transitions(records):
    """First-order Markov against the base rate."""
    models = [r["model"] for r in records]
    if len(models) < 3:
        return None

    counts = defaultdict(Counter)
    for a, b in zip(models, models[1:]):
        counts[a][b] += 1

    base = Counter(models).most_common(1)[0][0]
    base_hits = markov_hits = 0
    for a, b in zip(models, models[1:]):
        if base == b:
            base_hits += 1
        table = counts[a]
        if table and table.most_common(1)[0][0] == b:
            markov_hits += 1

    # Conditional entropy: how much uncertainty is left about the next model
    # once the current one is known, in bits.
    total_pairs = sum(sum(t.values()) for t in counts.values())
    conditional = 0.0
    for a, table in counts.items():
        n = sum(table.values())
        weight = n / total_pairs
        h = -sum((c / n) * math.log2(c / n) for c in table.values() if c)
        conditional += weight * h
    overall = Counter(models)
    total = sum(overall.values())
    marginal = -sum((c / total) * math.log2(c / total) for c in overall.values() if c)

    return {
        "n": len(models) - 1,
        "base_rate": base_hits / (len(models) - 1),
        "markov": markov_hits / (len(models) - 1),
        "entropy_marginal": marginal,
        "entropy_conditional": conditional,
        "distinct": len(overall),
    }


def prefill_rates(records):
    """Tokens per ms of prefill, per (node, model), from warm unqueued records.

    A warm record has no load in it, so its ttft is prefill alone -- the same
    bootstrap D19 uses. This exists because the engine reports no load duration
    on the chat path, so `load_ms` is null on every cold start and the cost of a
    load has to be recovered rather than read.
    """
    samples = defaultdict(list)
    for r in records:
        if not r.get("was_resident") or r.get("ttft_ms") is None:
            continue
        if r.get("inflight_at_dispatch"):
            continue
        window = r["ttft_ms"] - (r.get("queue_wait_ms") or 0)
        tokens = r.get("prompt_tokens_actual") or r.get("prompt_tokens_est")
        if window > 0 and tokens:
            samples[(r["node_id"], r["model"])].append(tokens / window)
    return {k: median(v) for k, v in samples.items()}


def load_cost(r, rates):
    """What this cold start actually paid to load, in ms.

    Returns (ms, source). `measured` when the engine reported it, `derived`
    when it came out of ttft with prefill subtracted, `predicted` when there is
    no warm baseline for that pair and only the router's own estimate remains.
    """
    if r.get("load_ms") and r["load_ms"] > 0:
        return r["load_ms"], "measured"
    rate = rates.get((r["node_id"], r["model"]))
    tokens = r.get("prompt_tokens_actual") or r.get("prompt_tokens_est")
    if rate and tokens and r.get("ttft_ms") is not None:
        derived = r["ttft_ms"] - (r.get("queue_wait_ms") or 0) - tokens / rate
        if derived > 0:
            return derived, "derived"
    for c in r.get("candidates", []):
        if c.get("node_id") == r.get("node_id") and c.get("t_load"):
            return c["t_load"], "predicted"
    return 0.0, "unknown"


def ceiling(records):
    busy_by_node = defaultdict(list)
    for r in records:
        busy_by_node[r["node_id"]].append((r["_disp"], r["_done"]))
    busy_by_node = {k: merge(v) for k, v in busy_by_node.items()}
    rates = prefill_rates(records)

    cold = []
    for index, r in enumerate(records):
        if r.get("was_resident") or index == 0:
            continue
        load_ms, source = load_cost(r, rates)
        if not load_ms:
            continue
        signal = records[index - 1]["_recv"]
        total_idle, longest_idle = idle_between(
            busy_by_node.get(r["node_id"], []), signal, r["_disp"])
        cold.append({
            "model": r["model"],
            "node": r["node_id"],
            "load_ms": load_ms,
            "source": source,
            "lead_total": total_idle,
            "lead_longest": longest_idle,
            "hidden_generous": min(total_idle, load_ms),
            "hidden_contiguous": min(longest_idle, load_ms),
        })
    return cold


def report(paths):
    runs = []
    for path in paths:
        records = load(path)
        if len(records) < 5:
            continue
        wall = (max(r["_done"] for r in records) -
                min(r["_recv"] for r in records))
        cold = ceiling(records)
        runs.append({"path": path, "records": records, "wall": wall,
                     "cold": cold})
    if not runs:
        print("no usable traces", file=sys.stderr)
        return 2

    every = [r for run in runs for r in run["records"]]
    print(f"{len(every)} ok records across {len(runs)} run(s)\n")

    print("IS THERE STRUCTURE TO LEARN?")
    stats = transitions(every)
    if not stats:
        print("  too few records")
    else:
        print(f"  {stats['distinct']} distinct model(s), {stats['n']} transitions")
        print(f"  base rate (always guess the commonest)  "
              f"{stats['base_rate'] * 100:5.1f} %")
        print(f"  first-order Markov (guess from previous) "
              f"{stats['markov'] * 100:5.1f} %")
        print(f"  entropy: {stats['entropy_marginal']:.2f} bits marginal -> "
              f"{stats['entropy_conditional']:.2f} bits given the previous model")
        gain = stats["markov"] - stats["base_rate"]
        print("  -> " + ("knowing the previous model helps"
                         if gain > 0.05 else
                         "knowing the previous model adds nothing worth having"))

    print("\nCOULD A PERFECT PREDICTOR ACT IN TIME?")
    all_cold = [c for run in runs for c in run["cold"]]
    if not all_cold:
        print("  no cold starts in these traces — nothing to hide")
        return 0

    fully = [c for c in all_cold if c["lead_total"] >= c["load_ms"]]
    fully_contig = [c for c in all_cold if c["lead_longest"] >= c["load_ms"]]
    saved = sum(c["hidden_generous"] for c in all_cold)
    saved_contig = sum(c["hidden_contiguous"] for c in all_cold)
    total_wall = sum(run["wall"] for run in runs)
    load_total = sum(c["load_ms"] for c in all_cold)

    sources = Counter(c["source"] for c in all_cold)
    print(f"  cold starts                {len(all_cold)}"
          f"  ({len(all_cold) / len(runs):.1f} per run)")
    print("  load cost from             " +
          ", ".join(f"{k} {v}" for k, v in sorted(sources.items())))
    print(f"  load time to hide          {load_total / 1000:8.1f} s"
          f"   ({load_total / total_wall * 100:.1f}% of all wall-clock)")
    print(f"  idle lead available, p50   "
          f"{median([c['lead_total'] for c in all_cold]):8.0f} ms"
          f"   vs load p50 {median([c['load_ms'] for c in all_cold]):.0f} ms")
    print(f"  fully hidden (summed idle) {len(fully):4d} / {len(all_cold)}")
    print(f"  fully hidden (contiguous)  {len(fully_contig):4d} / {len(all_cold)}"
          "   <- the honest one; a load does not pause and resume")
    print(f"  ceiling on wall-clock      {saved / 1000:8.1f} s generous, "
          f"{saved_contig / 1000:.1f} s contiguous")
    print(f"                             "
          f"{saved / total_wall * 100:8.1f} % generous, "
          f"{saved_contig / total_wall * 100:.1f} % contiguous")

    print("\n  per cold start (worst first):")
    for c in sorted(all_cold, key=lambda c: -c["load_ms"])[:8]:
        print(f"    {c['model']:<14} on {c['node']:<12} "
              f"load {c['load_ms']:7.0f} ms  lead {c['lead_total']:7.0f} ms "
              f"(contiguous {c['lead_longest']:7.0f})  "
              + ("hidden" if c["lead_longest"] >= c["load_ms"] else "NOT hidden"))

    print()
    if saved_contig / total_wall < 0.02:
        print("  -> Under assumptions chosen to flatter it, a perfect predictor")
        print("     recovers less than 2% of wall-clock. There is no room here.")
    return 0


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("traces", nargs="+")
    args = parser.parse_args()
    paths = []
    for pattern in args.traces:
        paths.extend(sorted(glob.glob(pattern)) or [pattern])
    return report(paths)


if __name__ == "__main__":
    sys.exit(main())
