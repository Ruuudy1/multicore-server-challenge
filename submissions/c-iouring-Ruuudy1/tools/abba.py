#!/usr/bin/env python3
# Paired A/B analysis of tools/localbench.py results (tests/bench-results.jsonl), the spec's decision rule:
# adopt B only if CPU us/request drops by >= 3% and the bootstrap 95% interval of the paired change excludes zero.
#   python3 tools/abba.py "<label A>" "<label B>" [results file]
# Runs are paired in the order they ran (A1 with B1, ...), which matches an ABBA schedule.
import json
import random
import sys

a_label, b_label = sys.argv[1], sys.argv[2]
path = sys.argv[3] if len(sys.argv) > 3 else "tests/bench-results.jsonl"
rows = [json.loads(line) for line in open(path) if line.strip()]
a = [r for r in rows if r["label"] == a_label]
b = [r for r in rows if r["label"] == b_label]
n = min(len(a), len(b))
if n == 0:
    sys.exit(f"no pairs for {a_label!r} / {b_label!r}")


def boot(deltas, reps=10000, seed=1):
    rng = random.Random(seed)
    means = sorted(sum(rng.choice(deltas) for _ in deltas) / len(deltas) for _ in range(reps))
    return means[int(0.025 * reps)], means[int(0.975 * reps)]


for metric, better in (("server_box_us_req", "lower"), ("rps", "higher"), ("p95", "lower"), ("p99", "lower")):
    deltas = [100.0 * (b[i][metric] - a[i][metric]) / a[i][metric] for i in range(n)]
    mean = sum(deltas) / n
    lo, hi = boot(deltas)
    print(f"{metric:18s} A {sum(x[metric] for x in a[:n]) / n:9.1f}  B {sum(x[metric] for x in b[:n]) / n:9.1f}  "
          f"change {mean:+6.1f}%  95% CI [{lo:+.1f}%, {hi:+.1f}%]  ({better} is better, {n} pairs)")
cpu = [100.0 * (b[i]["server_box_us_req"] - a[i]["server_box_us_req"]) / a[i]["server_box_us_req"] for i in range(n)]
lo, hi = boot(cpu)
adopt = sum(cpu) / n <= -3 and hi < 0
print(f"decision: {'adopt B' if adopt else 'keep A'} (CPU per request {sum(cpu) / n:+.1f}%, interval [{lo:+.1f}%, {hi:+.1f}%])")
