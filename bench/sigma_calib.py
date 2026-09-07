# -*- coding: utf-8 -*-
"""Candidate sigma rules, scored out of sample on recorded traces.

Every rule sees a record's prediction, produces a band from what earlier
records taught it, and is then shown the outcome -- in that order, so no rule is
ever scored against data it has already absorbed. What is being compared is not
"which band is widest" but which one is honest: coverage near 68% for a 1-sigma
band, and among the honest ones, the sharpest.
"""
import json, sys, glob, math, collections

HALFLIFE = 20.0
MIN_SAMPLES = 3
# MAD -> sigma for a normal. Chosen from the distribution, not fitted to the
# data, so the coverage below is a prediction being checked rather than a fit.
MAD_TO_SIGMA = 1.2533


class Ewma:
    __slots__ = ('value', 'samples')

    def __init__(self):
        self.value = 0.0
        self.samples = 0

    def add(self, x, hl=HALFLIFE):
        if not math.isfinite(x) or x <= 0:
            return
        if self.samples == 0:
            self.value = x
        else:
            w = 2.0 / (max(1.0, hl) + 1.0)
            self.value = w * x + (1.0 - w) * self.value
        self.samples += 1

    def has(self, n=MIN_SAMPLES):
        return self.samples >= n


def regime(w):
    if w['t_load'] > 0:
        return 'load'
    if w['t_queue'] > 0:
        return 'queue'
    return 'warm'


class Rule:
    def __init__(self, name):
        self.name = name
        self.cov = 0
        self.n = 0
        self.sigmas = []

    def score(self, rec, w, sigma):
        err = abs(rec['total_ms'] - rec['predicted_total_ms'])
        self.n += 1
        self.sigmas.append(sigma)
        if err <= sigma:
            self.cov += 1


class R0(Rule):
    """The band as recorded. On a trace written before D39 this is the analytic
    band alone, which is what the comparison is against."""
    def step(self, rec, w):
        return rec['predicted_sigma_ms']


class RAbs(Rule):
    """Learned absolute residual, keyed by node and regime."""
    def __init__(self, name, key):
        Rule.__init__(self, name)
        self.k = key
        self.e = collections.defaultdict(Ewma)

    def step(self, rec, w):
        k = self.k(rec, w)
        e = self.e[k]
        out = e.value * MAD_TO_SIGMA if e.has() else rec['predicted_sigma_ms']
        err = abs(rec['total_ms'] - rec['predicted_total_ms'])
        e.add(max(1.0, err))
        return out


class RRel(Rule):
    """Learned *relative* residual: sigma scales with the prediction."""
    def __init__(self, name, key, floor_ms=50.0):
        Rule.__init__(self, name)
        self.k = key
        self.floor = floor_ms
        self.e = collections.defaultdict(Ewma)

    def step(self, rec, w):
        k = self.k(rec, w)
        e = self.e[k]
        pred = max(self.floor, rec['predicted_total_ms'])
        out = e.value * pred * MAD_TO_SIGMA if e.has() else rec['predicted_sigma_ms']
        err = abs(rec['total_ms'] - rec['predicted_total_ms'])
        e.add(max(1e-3, err / pred))
        return out


class RFloor(Rule):
    """Today's analytic band, floored by the learned residual: keeps the
    structure the terms give it, refuses to claim more precision than the node
    has ever delivered."""
    def __init__(self, name, key):
        Rule.__init__(self, name)
        self.k = key
        self.e = collections.defaultdict(Ewma)

    def step(self, rec, w):
        k = self.k(rec, w)
        e = self.e[k]
        base = rec['predicted_sigma_ms']
        out = max(base, e.value * MAD_TO_SIGMA) if e.has() else base
        err = abs(rec['total_ms'] - rec['predicted_total_ms'])
        e.add(max(1.0, err))
        return out


class RRelFloor(Rule):
    """Same, but the floor scales with the prediction."""
    def __init__(self, name, key, floor_ms=50.0):
        Rule.__init__(self, name)
        self.k = key
        self.floor = floor_ms
        self.e = collections.defaultdict(Ewma)

    def step(self, rec, w):
        k = self.k(rec, w)
        e = self.e[k]
        pred = max(self.floor, rec['predicted_total_ms'])
        base = rec['predicted_sigma_ms']
        out = max(base, e.value * pred * MAD_TO_SIGMA) if e.has() else base
        err = abs(rec['total_ms'] - rec['predicted_total_ms'])
        e.add(max(1e-3, err / pred))
        return out


K_NODE_REGIME = lambda r, w: (r['node_id'], regime(w))
K_NODE = lambda r, w: r['node_id']
K_REGIME = lambda r, w: regime(w)
K_NODE_MODEL_REGIME = lambda r, w: (r['node_id'], r['model'], regime(w))


def run(patterns, label):
    rules = [
        R0('R0  as recorded in the trace'),
        RAbs('R1  abs resid  node+regime', K_NODE_REGIME),
        RRel('R2  rel resid  node+regime', K_NODE_REGIME),
        RRel('R3  rel resid  regime only', K_REGIME),
        RRel('R4  rel resid  node+model+regime', K_NODE_MODEL_REGIME),
        RFloor('R5  analytic, abs floor n+r  <-- ships', K_NODE_REGIME),
        RRelFloor('R6  analytic, rel floor n+r', K_NODE_REGIME),
    ]
    per_regime = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0]))

    n = 0
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
                n += 1
                err = abs(r['total_ms'] - r['predicted_total_ms'])
                for rule in rules:
                    s = rule.step(r, w)
                    rule.score(r, w, s)
                    cell = per_regime[rule.name][regime(w)]
                    cell[1] += 1
                    if err <= s:
                        cell[0] += 1

    def med(xs):
        xs = sorted(xs)
        return xs[len(xs) // 2] if xs else float('nan')

    print(f"\n########## {label}   (n={n}) ##########")
    print(f"{'rule':<30}{'coverage':>10}{'med sigma':>12}   per-regime coverage")
    for rule in rules:
        cov = rule.cov / max(1, rule.n) * 100.0
        pr = '  '.join(f"{k}:{v[0]/max(1,v[1])*100:3.0f}%({v[1]})"
                       for k, v in sorted(per_regime[rule.name].items()))
        print(f"{rule.name:<30}{cov:>9.0f}%{med(rule.sigmas):>12.0f}   {pr}")


if __name__ == '__main__':
    run(sys.argv[2:], sys.argv[1])
