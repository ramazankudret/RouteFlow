# -*- coding: utf-8 -*-
"""What happens to the machinery that only exists under concurrent load.

Every concurrent campaign in this project has run against simulated nodes,
whose queue and contention behaviour are modelled by assumption. This checks
the same mechanisms against a real engine, which has its own.

Two of these checks were wrong in their first version, in the way that matters:
they failed on correct behaviour. Both are noted where they sit, because a
check that cannot tell "the system is wrong" from "the workload never asked"
is worse than no check.
"""
import json, sys, collections, datetime

path = sys.argv[1]
subagents = int(sys.argv[2]) if len(sys.argv) > 2 else 4

records = []
for line in open(path, encoding='utf-8'):
    line = line.strip()
    if line:
        records.append(json.loads(line))

ok = [r for r in records if r.get('outcome') == 'ok']
passes = failures = unknown = 0


def ms(stamp):
    if not stamp:
        return None
    return datetime.datetime.strptime(stamp[:23], '%Y-%m-%dT%H:%M:%S.%f').replace(
        tzinfo=datetime.timezone.utc).timestamp() * 1000.0


def check(condition, label, detail=''):
    global passes, failures
    if condition:
        print('    PASS  %s' % label)
        passes += 1
    else:
        print('    FAIL  %s' % label)
        if detail:
            for line in detail.splitlines():
                print('          %s' % line)
        failures += 1


def inconclusive(label, detail):
    global unknown
    print('    ----  %s' % label)
    for line in detail.splitlines():
        print('          %s' % line)
    unknown += 1


def winner(r):
    return next((c for c in r.get('candidates', [])
                 if c['node_id'] == r['node_id'] and c.get('admitted')), None)


print('  %d records, %d served' % (len(records), len(ok)))

# 1. The baseline property. Everything else is worthless if requests are lost.
bad = [r for r in records if r.get('outcome') != 'ok']
check(not bad and len(ok) > 0,
      'every request under concurrency is served',
      '%d not ok: %s' % (len(bad), collections.Counter(
          r.get('outcome') for r in bad).most_common(3)))

# 2. Did concurrency actually reach the router? Without this the rest would
#    pass vacuously on a workload that quietly serialised.
overlapped = [r for r in ok if (r.get('inflight_at_dispatch') or 0) > 0]
check(len(overlapped) >= 2,
      'requests actually overlapped at the router',
      'only %d of %d records saw anything else in flight' % (len(overlapped), len(ok)))

# 3. T_queue: a node already at its slot count must be priced for the wait
#    rather than as though the request would start immediately (§6.2).
busy = [r for r in ok if (r.get('inflight_at_dispatch') or 0) >= 1 and winner(r)]
queued = [r for r in busy if winner(r)['t_queue'] > 0]
check(len(queued) > 0,
      'the queue term fires on a busy node',
      '%d records were dispatched into a busy node and none was priced for it'
      % len(busy))

# 4. Contention (D5). alpha is learned only from records whose
#    `concurrent_decoders_at_dispatch` exceeds one -- a sample taken at
#    dispatch. So the question is not "did the router record contention" but
#    "was there contention to record", and the trace can answer it directly by
#    intersecting the decode windows.
by_node = collections.defaultdict(list)
for r in ok:
    start, end = ms(r.get('ts_first_token')), ms(r.get('ts_done'))
    if start and end:
        by_node[r['node_id']].append((start, end, r))

really_overlapped = 0
for node, spans in by_node.items():
    spans.sort()
    for i in range(len(spans)):
        for j in range(i + 1, len(spans)):
            if spans[j][0] < spans[i][1]:
                really_overlapped += 1
            else:
                break

# D44: the count that matters is taken at the first token. The dispatch field
# is still read as a fallback, because a trace written before D44 has only that.
def decoders(r):
    return (r.get('concurrent_decoders_at_first_token')
            or r.get('concurrent_decoders_at_dispatch') or 0)


recorded = [r for r in ok if decoders(r) > 1]
if really_overlapped == 0:
    inconclusive('contention was never produced, so alpha could not learn',
                 'No two replies decoded at the same time on one node. Nothing\n'
                 'to record; this workload did not ask the question.')
else:
    check(len(recorded) > 0,
          'contention that happened was recorded, so alpha has evidence',
          '%d pairs of replies genuinely decoded at the same time on one node,\n'
          'and %d records report more than one decoder.\n'
          'concurrent_decoders is sampled at dispatch, so a burst that starts\n'
          'together always sees zero -- which is the shape of the agent\n'
          'workload this project is built for: a planner fanning out to N\n'
          'sub-agents at once.' % (really_overlapped, len(recorded)))

# 5. The herd (§6.3, D4). A request arriving *while* a load is already running
#    should be priced for the remainder, not for a fresh load.
#
#    The first version of this compared every load of the same node+model in
#    the run and failed when they were equal. They were equal because the
#    requests were dispatched in the same millisecond, where a full remaining
#    load is the correct price. Only a request that arrives measurably later
#    tests anything.
loads = []
for r in ok:
    w = winner(r)
    if w and not r.get('was_resident') and w['t_load'] > 0:
        loads.append((ms(r['ts_dispatched']), r, w))
loads.sort(key=lambda x: x[0])

mid_load = []
for i, (t_i, r_i, w_i) in enumerate(loads):
    for t_j, r_j, w_j in loads[i + 1:]:
        if r_j['node_id'] != r_i['node_id'] or r_j['model'] != r_i['model']:
            continue
        gap = t_j - t_i
        if 50 < gap < w_i['t_load']:      # arrived while the first was loading
            mid_load.append((gap, w_i['t_load'], w_j['t_load']))

if not mid_load:
    inconclusive('no request arrived while another was mid-load',
                 'Every concurrent load began in the same millisecond, where\n'
                 'pricing a full remaining load is correct. D4 is untested by\n'
                 'this run rather than failing it.')
else:
    good = [g for g in mid_load if g[2] < g[1] * 0.95]
    check(len(good) == len(mid_load),
          'a request arriving mid-load is priced for the remainder, not a '
          'fresh load',
          'gaps and prices: %s' % mid_load[:4])

# 6. Nothing was admitted onto a node that then could not serve it.
oom = [r for r in records
       if 'insufficient' in str(r.get('error', '')).lower()
       or (r.get('outcome') or '') == 'dispatch_failed']
check(not oom,
      'no request was sent to a node that then could not serve it',
      '%d such records' % len(oom))

print('\n  %d passed, %d failed, %d inconclusive' % (passes, failures, unknown))
sys.exit(0 if failures == 0 else 1)
