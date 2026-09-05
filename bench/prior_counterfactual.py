#!/usr/bin/env python3
"""Would a better fixed prior have been enough?

The learned cost model beats the static one on an uncapped workload largely by
predicting output length instead of assuming 256 tokens. The obvious objection
is that 256 was simply the wrong constant, and that a hand-tuned prior would
have closed most of the gap without any learning at all.

That is answerable from the trace and does not need another run. Every record
carries the winner's term breakdown (D10), and `t_decode` is linear in the
predicted token count, so substituting a different prior P is exact:

    t_decode'      = t_decode * P / predicted_output_tokens
    predicted_ms'  = predicted_ms - t_decode + t_decode'

This replays the *prediction* only. Routing is left as it happened, because a
different prior would also have changed which node won and that cannot be
replayed without re-running the cluster. So the number below is the best case
for a tuned prior: it gets the accuracy benefit with none of the routing risk.

  python3 bench/prior_counterfactual.py bench/results-phase2-uncapped/static-v1-*.jsonl
"""

import argparse
import glob
import json
import math
import sys
from collections import defaultdict


def load(paths):
    records = []
    for path in paths:
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
                if not r.get("predicted_total_ms") or not r.get("total_ms"):
                    continue
                if not r.get("predicted_output_tokens"):
                    continue
                records.append(r)
    return records


def median(values):
    if not values:
        return None
    s = sorted(values)
    mid = len(s) // 2
    return s[mid] if len(s) % 2 else (s[mid - 1] + s[mid]) / 2.0


def winner_decode_ms(record):
    """The winning candidate's t_decode, which is what the prior scaled."""
    for c in record.get("candidates", []):
        if c.get("node_id") == record.get("node_id") and c.get("admitted"):
            return c.get("t_decode")
    return None


def error_with_prior(records, prior_for):
    """Median |predicted - actual| / actual after substituting a prior."""
    errors = []
    for r in records:
        decode = winner_decode_ms(r)
        if decode is None:
            continue
        prior = prior_for(r)
        assumed = r["predicted_output_tokens"]
        if not assumed or not prior:
            continue
        scaled = decode * float(prior) / float(assumed)
        predicted = r["predicted_total_ms"] - decode + scaled
        actual = r["total_ms"]
        if actual > 0:
            errors.append(abs(predicted - actual) / actual)
    return median(errors), len(errors)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("traces", nargs="+")
    args = parser.parse_args()

    paths = []
    for pattern in args.traces:
        paths.extend(sorted(glob.glob(pattern)) or [pattern])
    records = load(paths)
    if not records:
        print("no usable records", file=sys.stderr)
        return 2

    assumed = sorted({r["predicted_output_tokens"] for r in records})
    print(f"{len(records)} records from {len(paths)} trace(s)")
    print(f"prior as run: {assumed}")

    # What the replies actually were, which is the number a tuned prior would
    # have to have been set to — after the fact.
    by_model = defaultdict(list)
    for r in records:
        if r.get("output_tokens"):
            by_model[r["model"]].append(r["output_tokens"])
    print("\nactual output length:")
    for model in sorted(by_model):
        lengths = by_model[model]
        print(f"  {model:<14} n {len(lengths):>3}  "
              f"mean {sum(lengths) / len(lengths):>6.1f}  "
              f"median {median(lengths):>6.1f}  "
              f"range {min(lengths)}-{max(lengths)}")

    base, n = error_with_prior(records, lambda r: r["predicted_output_tokens"])
    print(f"\ntiming error as run          {base * 100:>7.1f} %   (n {n})")

    # One constant for the whole cluster, chosen with hindsight.
    everything = [x for xs in by_model.values() for x in xs]
    one = median(everything)
    tuned, _ = error_with_prior(records, lambda r: one)
    print(f"tuned single prior ({one:>5.0f} tok) {tuned * 100:>7.1f} %"
          f"   <- one constant, chosen after seeing the answers")

    # A different constant per model, still chosen with hindsight. This is the
    # most a fixed prior can possibly do on this workload.
    per_model = {m: median(v) for m, v in by_model.items()}
    best, _ = error_with_prior(records, lambda r: per_model.get(r["model"]))
    print(f"tuned per-model prior        {best * 100:>7.1f} %"
          f"   <- the ceiling for any fixed prior here")
    print("  " + ", ".join(f"{m}={p:.0f}" for m, p in sorted(per_model.items())))

    # And the floor: knowing each reply's length exactly, in advance.
    oracle, _ = error_with_prior(records, lambda r: r.get("output_tokens"))
    print(f"oracle (actual length)       {oracle * 100:>7.1f} %"
          f"   <- what is left when length is perfect")
    print("\nWhatever is left at the oracle line is not a length problem; it is "
          "the rest of the model.")

    # The accuracy number is not the interesting one. T_decode is the largest
    # term, so an inflated length inflates it until it swamps T_load - and a
    # warmth-aware router quietly stops being warmth-aware. Re-rank every
    # decision with each candidate's t_decode scaled to the length the reply
    # actually turned out to have.
    flips = warmer = ranked = 0
    for r in records:
        actual = r.get("output_tokens")
        assumed = r["predicted_output_tokens"]
        if not actual or not assumed:
            continue
        admitted = [c for c in r.get("candidates", []) if c.get("admitted")]
        if len(admitted) < 2:
            continue
        ranked += 1

        def rescored(c, actual=actual, assumed=assumed):
            decode = c.get("t_decode") or 0.0
            return c["predicted_total_ms"] - decode + decode * actual / assumed

        chosen = next((c for c in admitted
                       if c.get("node_id") == r.get("node_id")), None)
        pick = min(admitted, key=rescored)
        if chosen is None or pick.get("node_id") == r.get("node_id"):
            continue
        flips += 1
        # A flip only helps if it lands somewhere already holding the model.
        if (pick.get("t_load") or 0) == 0 and (chosen.get("t_load") or 0) > 0:
            warmer += 1

    if ranked:
        print("\nrouting counterfactual over %d decision(s) with a choice:" % ranked)
        print("  scaling t_decode to the true length moves %d/%d of them (%.0f%%)"
              % (flips, ranked, flips / ranked * 100))
        print("  %d of those move from a cold node to a warm one" % warmer)
        if flips:
            print("  -> the length prior was not merely inaccurate, it was "
                  "outranking warmth")
    return 0


if __name__ == "__main__":
    sys.exit(main())
