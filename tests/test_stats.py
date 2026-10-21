#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Checks DTO_COLLECT_STATS output against test_stats_workload's known calls.
# Needs no DSA hardware: without it every call is counted on the CPU path.
#
# Usage: test_stats.py <test_stats_workload> <libdto.so>

import json
import os
import subprocess
import sys
import tempfile

WORKLOAD = sys.argv[1]
LIBDTO = os.path.abspath(sys.argv[2])
SIZES = {"memcpy": 131072 + 123, "memset": 65536 + 4096,
         "memmove": 98304 + 7, "memcmp": 262144 + 64}
SIZE_CHILD = 524288 + 1
failures = 0


def check(desc, ok, detail=""):
    global failures
    print(("PASS: " if ok else "FAIL: ") + desc + ("" if ok else "  " + detail))
    failures += not ok


def run(args, env_extra, stats_path):
    env = dict(os.environ, DTO_COLLECT_STATS="1", DTO_STATS_FILE=stats_path,
               **env_extra)
    out = subprocess.run([WORKLOAD] + args, env=env, check=True,
                         capture_output=True, text=True).stdout
    with open(stats_path) as f:
        return out, f.read()


def records(text):
    return [json.loads(line) for line in text.splitlines() if line.strip()]


def op_sum(ops, op, field):
    return sum(g[field] for g in ops.get(op, {}).values())


def bucket_for(summary, size):
    for b in summary["histogram"]:
        if b["min"] <= size and ("max" not in b or size <= b["max"]):
            return b
    return None


tmp = tempfile.mkdtemp()

# --- totals and histogram, JSON ---
threads, iters = 4, 50
_, text = run(["basic", str(threads), str(iters)], {"DTO_STATS_FORMAT": "json"},
              os.path.join(tmp, "basic.json"))
recs = records(text)
summaries = [r for r in recs if r["type"] == "summary"]
check("one summary record", len(summaries) == 1, str(len(summaries)))
s = summaries[0]
n = threads * iters
for op, size in SIZES.items():
    b = bucket_for(s, size)
    check(f"{op} lands in the bucket for {size} bytes", b is not None and op in b["ops"],
          json.dumps(b))
    if b is None or op not in b["ops"]:
        continue
    calls = op_sum(b["ops"], op, "calls")
    failed = b["ops"][op].get("dsa_failed", {}).get("calls", 0)
    # a call the device left partly done is counted as dsa_failed plus cpu
    check(f"{op} calls ({n})", calls - failed == n, f"calls {calls} failed {failed}")
    check(f"{op} bytes ({n * size})", op_sum(b["ops"], op, "bytes") == n * size,
          str(op_sum(b["ops"], op, "bytes")))
    check(f"{op} time recorded", op_sum(b["ops"], op, "ns") > 0)
check("summary carries the configuration", "wait_method" in s["config"])

# --- interval records add up to the totals ---
_, text = run(["basic", "2", "40", "2"],
              {"DTO_STATS_FORMAT": "json", "DTO_STATS_INTERVAL_MS": "10"},
              os.path.join(tmp, "interval.json"))
recs = records(text)
intervals = [r for r in recs if r["type"] == "interval"]
s = [r for r in recs if r["type"] == "summary"][0]
check("interval records emitted", len(intervals) >= 5, str(len(intervals)))
check("interval times increase",
      all(b["t_ms"] > a["t_ms"] for a, b in zip(intervals, intervals[1:])))
for op in SIZES:
    for field in ("calls", "bytes"):
        total = sum(op_sum(r["ops"], op, field) for r in intervals)
        check(f"intervals sum to the {op} {field} total",
              total == op_sum(s["totals"], op, field),
              f"{total} vs {op_sum(s['totals'], op, field)}")

# --- short-lived threads: all counted, blocks released ---
out, text = run(["churn", "200", "5"], {"DTO_STATS_FORMAT": "json"},
                os.path.join(tmp, "churn.json"))
s = [r for r in records(text) if r["type"] == "summary"][0]
b = bucket_for(s, SIZES["memcpy"])
calls = op_sum(b["ops"], "memcpy", "calls") - \
    b["ops"]["memcpy"].get("dsa_failed", {}).get("calls", 0)
check("calls from 200 exited threads counted", calls == 200 * 5, str(calls))
before, after = (int(x) for x in out.split()[1::2])
check("exited threads' stats blocks are unmapped", after - before < 20,
      f"mappings {before} -> {after}")

# --- fork: separate records per process, appended to one file ---
out, text = run(["fork", "30"], {"DTO_STATS_FORMAT": "json"},
                os.path.join(tmp, "fork.json"))
child = int(out.split()[1])
by_pid = {r["pid"]: r for r in records(text) if r["type"] == "summary"}
check("parent and child both report, in one file", len(by_pid) == 2, str(list(by_pid)))
if child in by_pid and len(by_pid) == 2:
    parent = [p for p in by_pid if p != child][0]
    cb = bucket_for(by_pid[child], SIZE_CHILD)
    check("child counts only its own copies",
          cb is not None and bucket_for(by_pid[child], SIZES["memcpy"]) is None)
    pb = bucket_for(by_pid[parent], SIZES["memcpy"])
    check("parent counts its copies from before and after the fork",
          pb is not None and op_sum(pb["ops"], "memcpy", "bytes") == 60 * SIZES["memcpy"])

# --- %p: a file per process ---
pattern = os.path.join(tmp, "pp.%p.json")
env = dict(os.environ, DTO_COLLECT_STATS="1", DTO_STATS_FILE=pattern,
           DTO_STATS_FORMAT="json")
subprocess.run([WORKLOAD, "fork", "5"], env=env, check=True, capture_output=True)
files = [f for f in os.listdir(tmp) if f.startswith("pp.")]
check("%p gives each process its own file", len(files) == 2, str(files))

# --- a program that doesn't link libpthread (before glibc 2.34, it isn't libc) ---
env = dict(os.environ, LD_PRELOAD=LIBDTO, DTO_COLLECT_STATS="1",
           DTO_STATS_INTERVAL_MS="1", DTO_STATS_FILE=os.path.join(tmp, "true.txt"))
p = subprocess.run(["true"], env=env, capture_output=True, text=True)
check("stats work in a program without libpthread", p.returncode == 0, p.stderr.strip())

# --- text format ---
_, text = run(["basic", "1", "10"], {"DTO_STATS_FORMAT": "text"},
              os.path.join(tmp, "basic.txt"))
check("text summary has the operation table", "Number of Memory Operations" in text
      and "Byte Range" in text)
check("text summary lists the memcpy bucket", "  131072-163839" in text)

print()
if failures:
    print(f"{failures} stats test(s) failed")
    sys.exit(1)
print("All stats tests passed")
