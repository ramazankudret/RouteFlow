# -*- coding: utf-8 -*-
"""Did the retry go somewhere else?

§6.4 allows one retry before the first byte reaches the client, and D9 says it
goes to another node. Whether it *did* is not something a unit test can see:
the exclusion list is assembled in the dispatcher and consumed by admission,
and the defect D42 fixed was that the two were never connected. Only a run
where a node genuinely fails can tell them apart.
"""
import json, sys

records = {}
order = []
for line in open(sys.argv[1], encoding='utf-8'):
    line = line.strip()
    if not line:
        continue
    r = json.loads(line)
    records[r['job_id']] = r
    order.append(r)

retries = [r for r in order if r.get('retry_of')]
same, moved, orphan = 0, 0, 0
for r in retries:
    prev = records.get(r['retry_of'])
    if prev is None:
        orphan += 1
    elif prev.get('node_id') == r.get('node_id'):
        same += 1
    else:
        moved += 1

print("RETRIES total=%d moved=%d same_node=%d unmatched=%d"
      % (len(retries), moved, same, orphan))
