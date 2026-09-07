# -*- coding: utf-8 -*-
"""Where does the total-time residual come from?

Load and prefill are only separable when the engine reported a load time. On
the OpenAI-compatible path it does not, so those records are compared as one
term -- ttft against t_load + t_prefill -- rather than pretending the split is
known. Charging an unmeasured load to prefill is how the first version of this
script invented a 1.5 s prefill bias that was not there.
"""
import json, sys, glob, collections


def pct(xs, q):
    if not xs:
        return float('nan')
    xs = sorted(xs)
    return xs[min(len(xs) - 1, max(0, int(round(q / 100.0 * (len(xs) - 1)))))]


def rows_from(patterns):
    out = []
    for pat in patterns:
        for p in sorted(glob.glob(pat)):
            for line in open(p, encoding='utf-8'):
                line = line.strip()
                if not line:
                    continue
                r = json.loads(line)
                if r.get('outcome') != 'ok' or not r.get('node_id'):
                    continue
                w = next((c for c in r.get('candidates', [])
                          if c['node_id'] == r['node_id'] and c.get('admitted')), None)
                if not w:
                    continue
                q_a = r.get('queue_wait_ms')
                l_a = r.get('load_ms')          # None => not measured
                ttft = r.get('ttft_ms')
                out.append(dict(
                    node=r['node_id'], model=r['model'], res=r.get('was_resident'),
                    total=r['total_ms'], pred=r['predicted_total_ms'],
                    sig=r['predicted_sigma_ms'],
                    err=abs(r['total_ms'] - r['predicted_total_ms']),
                    q_a=(q_a or 0.0), q_p=w['t_queue'],
                    l_a=l_a, l_p=w['t_load'],
                    ttft=ttft, pf_p=w['t_prefill'], dec_p=w['t_decode'],
                    cold=(w['t_load'] > 0 or (l_a or 0) > 0 or not r.get('was_resident')),
                    queued=(w['t_queue'] > 0 or (q_a or 0) > 0),
                ))
    return out


def report(name, rows):
    if not rows:
        print(f'{name}: nothing usable'); return
    n = len(rows)
    cov = sum(1 for x in rows if x['err'] <= x['sig']) / n * 100.0
    print(f"\n=== {name}  (n={n}) ===")
    print(f"  |error| p50 {pct([x['err'] for x in rows],50):7.0f}   "
          f"sigma p50 {pct([x['sig'] for x in rows],50):7.0f}   "
          f"coverage {cov:3.0f}%   (calibrated 1-sigma ~68%)")

    def line(label, xs, a, p):
        if not xs:
            print(f"  {label:<30} --"); return
        e = [abs(ai - pi) for ai, pi in zip(a, p)]
        s = [ai - pi for ai, pi in zip(a, p)]
        print(f"  {label:<30} n={len(xs):<4} |err| p50 {pct(e,50):7.0f}"
              f"  p95 {pct(e,95):7.0f}   bias(med) {pct(s,50):+8.0f} ms")

    print('  --- residual by term (only where actual and predicted are comparable) ---')
    xs = rows
    line('queue', xs, [x['q_a'] for x in xs], [x['q_p'] for x in xs])

    # ttft = load + prefill. Separable only when the engine reported the load.
    sep = [x for x in rows if x['l_a'] is not None and x['ttft'] is not None]
    line('load (engine-reported)', sep, [x['l_a'] for x in sep], [x['l_p'] for x in sep])
    line('prefill (ttft-load-queue)', sep,
         [x['ttft'] - x['l_a'] - x['q_a'] for x in sep], [x['pf_p'] for x in sep])

    uns = [x for x in rows if x['l_a'] is None and x['ttft'] is not None]
    line('load+prefill (unsplit)', uns,
         [x['ttft'] - x['q_a'] for x in uns], [x['l_p'] + x['pf_p'] for x in uns])

    dec = [x for x in rows if x['ttft'] is not None]
    line('decode (total-ttft)', dec,
         [x['total'] - x['ttft'] for x in dec], [x['dec_p'] for x in dec])

    print('  --- coverage by regime ---')
    for label, sub in (('warm, unqueued', [x for x in rows if not x['cold'] and not x['queued']]),
                       ('cold', [x for x in rows if x['cold']]),
                       ('queued', [x for x in rows if x['queued']])):
        if not sub:
            continue
        c = sum(1 for x in sub if x['err'] <= x['sig']) / len(sub) * 100.0
        ratio = pct([x['err'] for x in sub], 50) / max(1e-9, pct([x['sig'] for x in sub], 50))
        print(f"  {label:<30} n={len(sub):<4} |err| p50 {pct([x['err'] for x in sub],50):7.0f}"
              f"   sigma p50 {pct([x['sig'] for x in sub],50):7.0f}"
              f"   ratio {ratio:5.1f}x   coverage {c:3.0f}%")

    print('  --- how often is the load measured at all? ---')
    coldrows = [x for x in rows if x['cold']]
    measured = sum(1 for x in coldrows if x['l_a'] is not None)
    print(f"  cold records {len(coldrows)}, engine reported a load time for {measured}")


if __name__ == '__main__':
    report(sys.argv[1], rows_from(sys.argv[2:]))
