# -*- coding: utf-8 -*-
"""Is there a routing that keeps the wall-clock win and does not wreck TTFT?

Warmth routes to nodes that must load, and a load lands in front of the first
token: on the real two-node cluster its ttft p95 is more than double
RoundRobin's. §9 makes wall-clock the criterion for agent workloads, where
nobody watches an intermediate reply -- but that is an assumption about the
caller, not a fact about the cluster, and it has never been checked against the
alternative.

D10 records every candidate's estimate, including the losers'. So the question
"what would a different objective have chosen" is answerable on traces already
taken, before writing any policy:

    ttft_est  = t_queue + t_load + t_prefill
    total_est = ttft_est + t_decode + t_evict

What this can and cannot say. It reports what the *cost model* believed at
decision time, so it answers "would the ranking have differed, and by how much
did the model think" -- not "what would the clock have done". That is enough to
decide whether anything is worth building, and not enough to publish a win.
"""
import json, sys, glob, statistics as st


def pct(xs, q):
    if not xs:
        return float('nan')
    xs = sorted(xs)
    return xs[min(len(xs) - 1, max(0, int(round(q / 100.0 * (len(xs) - 1)))))]


def ttft_of(c):
    return c['t_queue'] + c['t_load'] + c['t_prefill']


def total_of(c):
    return c['predicted_total_ms']


def load(patterns):
    out = []
    for pat in patterns:
        for p in sorted(glob.glob(pat)):
            for line in open(p, encoding='utf-8'):
                line = line.strip()
                if not line:
                    continue
                r = json.loads(line)
                if r.get('outcome') != 'ok':
                    continue
                cands = [c for c in r.get('candidates', []) if c.get('admitted')]
                if len(cands) < 2:
                    continue
                out.append((r, cands))
    return out


def main(label, patterns):
    rows = load(patterns)
    if not rows:
        print(f'{label}: no records with a choice to make')
        return 1

    disagree = []
    for r, cands in rows:
        by_total = min(cands, key=total_of)
        by_ttft = min(cands, key=ttft_of)
        if by_total['node_id'] != by_ttft['node_id']:
            disagree.append((r, by_total, by_ttft))

    n = len(rows)
    d = len(disagree)
    print(f'\n=== {label} ===')
    print(f'  requests with a real choice: {n}')
    print(f'  min-total and min-ttft disagree: {d} ({d * 100.0 / n:.0f}%)')
    if not disagree:
        print('  Nothing to trade. A ttft-aware objective would route identically.')
        return 0

    gain = [ttft_of(bt) - ttft_of(bf) for _, bt, bf in disagree]     # ttft saved
    cost = [total_of(bf) - total_of(bt) for _, bt, bf in disagree]   # total paid
    print(f'  where they differ, the model expects:')
    print(f'     ttft saved   p50 {pct(gain,50):7.0f}  p95 {pct(gain,95):7.0f} ms')
    print(f'     total paid   p50 {pct(cost,50):7.0f}  p95 {pct(cost,95):7.0f} ms')
    ratio = [c / g for g, c in zip(gain, cost) if g > 0]
    if ratio:
        print(f'     ms of total per ms of ttft: p50 {pct(ratio,50):.2f}')

    # How far to trust any of this. The objective is a ranking, but the numbers
    # below are the model's, and on this cluster its timing error is ~34%.
    pred = [ttft_of(min(c, key=total_of)) for _, c in rows]
    act = [r['ttft_ms'] for r, _ in rows if r.get('ttft_ms') is not None]
    print(f'  calibration check on the node actually chosen:')
    print(f'     predicted ttft p50 {pct(pred,50):7.0f}  p95 {pct(pred,95):7.0f} ms')
    print(f'     measured  ttft p50 {pct(act,50):7.0f}  p95 {pct(act,95):7.0f} ms')

    # The frontier. score = total + w * ttft; w = 0 is what ships, w -> inf is
    # min-ttft. If there is a knee, a knob is worth building; if it is a
    # straight line, the trade is real and the choice belongs to the operator.
    print('  %6s%16s%10s%10s%14s' % ('w', 'pred total (s)', 'ttft p50',
                                       'ttft p95', 'vs w=0 total'))
    base = None
    for w in (0.0, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 1e9):
        chosen = [min(c, key=lambda x: total_of(x) + w * ttft_of(x)) for _, c in rows]
        tot = sum(total_of(c) for c in chosen) / 1000.0
        if base is None:
            base = tot
        t = [ttft_of(c) for c in chosen]
        tag = 'min-ttft' if w > 1e8 else f'{w:.2f}'
        print(f'  {tag:>6}{tot:>16.1f}{pct(t,50):>10.0f}{pct(t,95):>10.0f}'
              f'{(tot - base) / base * 100:>13.0f}%')

    # The sum above is the batch's cost, which is what §9 optimises for an agent
    # workload. Someone waiting on their own reply cares about a different
    # number: how long *their* request takes end to end. If chasing the first
    # token also delays the last one, there is nobody the trade serves.
    print('  per-request, for the caller who is actually watching:')
    print('  %8s%14s%14s%14s' % ('w', 'ttft p50', 'total p50', 'total p95'))
    for w in (0.0, 1.0, 1e9):
        chosen = [min(c, key=lambda x: total_of(x) + w * ttft_of(x)) for _, c in rows]
        tag = 'min-ttft' if w > 1e8 else f'{w:.2f}'
        print('  %8s%14.0f%14.0f%14.0f' % (
            tag, pct([ttft_of(c) for c in chosen], 50),
            pct([total_of(c) for c in chosen], 50),
            pct([total_of(c) for c in chosen], 95)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2:]))
