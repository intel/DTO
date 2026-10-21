#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Checks dto-stats-report's arithmetic on made-up stats runs with known
# answers, its perf sample categories on made-up "perf report" lines, and
# that it finds libc's mem* functions by address in this machine's libc.
# Needs neither DSA nor perf.
#
# Usage: test_stats_report.py <dto-stats-report>

import ctypes
import importlib.machinery
import importlib.util
import json
import os
import subprocess
import sys
import tempfile

REPORT = sys.argv[1]
failures = 0
sys.dont_write_bytecode = True      # importing the report must not litter


def check(desc, ok, detail=""):
    global failures
    print(("PASS: " if ok else "FAIL: ") + desc + ("" if ok else "  " + str(detail)))
    failures += not ok


def close(a, b):
    return a is not None and abs(a - b) <= 1e-6 * max(1, abs(b))


def ops(**kw):
    """ops(memcpy_cpu=(calls, bytes, ns), ...) in the summary's layout"""
    out = {}
    for key, (calls, nbytes, ns) in kw.items():
        op, path = key.split("_", 1)
        out.setdefault(op, {})[path] = {"calls": calls, "bytes": nbytes, "ns": ns}
    return out


def summary(pid, totals, hist, stdc_only=0, page_faults=0):
    return {"type": "summary", "pid": pid, "program": "app", "run_ms": 1.0,
            "config": {"stdc_only": stdc_only, "wait_method": "busypoll",
                       "min_bytes": 65536, "cpu_size_fraction": 0.0,
                       "auto_adjust": 1, "numa_aware": 0, "wqs": 1},
            "totals": totals,
            "failures": {"retry": 0, "page_fault": page_faults, "other": 0},
            "histogram": hist}


def write_run_dir(runs):
    """runs: (name, mode, wall_s, cpu_s, [summary records], exit_status)"""
    d = tempfile.mkdtemp()
    with open(os.path.join(d, "runs.jsonl"), "w") as f:
        f.write(json.dumps({"type": "stats_run", "library": "/x/libdto.so",
                            "host": "test", "repeat": 1, "perf": None,
                            "command": ["app"], "env": {}}) + "\n")
        for name, mode, wall, cpu, recs, status in runs:
            f.write(json.dumps({"type": "run", "name": name, "mode": mode,
                                "round": 1, "profiled": False,
                                "stats": name + ".jsonl", "wall_s": wall,
                                "user_s": cpu, "sys_s": 0.0, "maxrss_kb": 1,
                                "exit_status": status}) + "\n")
            with open(os.path.join(d, name + ".jsonl"), "w") as s:
                for r in recs:
                    s.write(json.dumps(r) + "\n")
    return d


def report(d):
    p = subprocess.run([REPORT, "--json", d], capture_output=True, text=True)
    if p.returncode:
        print(p.stderr)
        return None
    return json.loads(p.stdout)


MiB = 1 << 20
B_CPY, B_SET = (1024 * MiB, 512 * MiB)
H_CPY = (1 * MiB, 1 * MiB + MiB // 4 - 1)
H_SET = (256 << 10, (320 << 10) - 1)


def hist(cpy, mset):
    return [{"min": H_SET[0], "max": H_SET[1], "ops": mset},
            {"min": H_CPY[0], "max": H_CPY[1], "ops": cpy}]


# CPU-only: memcpy 4 s and memset 1 s of 10 s CPU time, so f = 0.5
cpu_cpy = ops(memcpy_cpu=(1000, B_CPY, 4e9))
cpu_set = ops(memset_cpu=(1000, B_SET, 1e9))
cpu_rec = summary(1, {**cpu_cpy, **cpu_set}, hist(cpu_cpy, cpu_set))
# with DSA: memcpy 4x faster; memset 90% of bytes on DSA, 0.3 s in all.
# Two processes, each reporting part of the work.
dsa_cpy = ops(memcpy_dsa=(1000, B_CPY, 1e9))
dsa_set = ops(memset_dsa=(900, B_SET * 9 // 10, 2e8),
              memset_cpu=(100, B_SET // 10, 1e8))
dsa_recs = [summary(2, dsa_cpy, hist(dsa_cpy, {}), page_faults=5),
            summary(3, dsa_set, hist({}, dsa_set))]

# --- the numbers ---
d = write_run_dir([
    ("cpu-1", "cpu", 10.0, 10.0, [cpu_rec], 0),
    ("dsa-1", "dsa", 6.0, 6.0, dsa_recs, 0),
    ("cpu-2", "cpu", 30.0, 10.0, [cpu_rec], 0),     # an outlier
    ("dsa-2", "dsa", 7.0, 6.2, dsa_recs, 0),
    ("cpu-3", "cpu", 11.0, 10.0, [cpu_rec], 0),
    ("dsa-3", "dsa", 6.5, 6.1, dsa_recs, 0),
])
r = report(d)
check("report runs", r is not None)
if r:
    a = r["amdahl"]
    s = 5.0 / 1.3
    check("f = mem* share of CPU-only CPU time", close(a["f"], 0.5), a["f"])
    check("s = mem* speedup", close(a["s"], s), a["s"])
    check("predicted = 1 / ((1 - f) + f / s)", close(a["predicted"], 1 / (0.5 + 0.5 / s)),
          a["predicted"])
    check("limit = 1 / (1 - f)", close(a["limit"], 2.0), a["limit"])
    check("measured speedup uses median wall times", close(r["measured"]["wall"], 11 / 6.5),
          r["measured"])
    check("median CPU time", close(r["modes"]["dsa"]["cpu_s"], 6.1), r["modes"]["dsa"])
    check("processes of a run are summed", close(r["modes"]["dsa"]["mem_s"], 1.3),
          r["modes"]["dsa"]["mem_s"])
    o = r["ops"]
    check("memcpy speedup", close(o["memcpy"]["speedup"], 4.0), o["memcpy"])
    check("memset speedup", close(o["memset"]["speedup"], 1 / 0.3), o["memset"])
    check("memset share on DSA", close(o["memset"]["on_dsa"], 0.9), o["memset"])
    check("memcpy share of CPU time", close(o["memcpy"]["share"], 0.4), o["memcpy"])
    check("memset calls", o["memset"]["calls"] == 1000, o["memset"])
    check("page faults per run", close(r["dsa_failures"]["page_fault"], 5),
          r["dsa_failures"])
    b = {x["min"]: x for x in r["buckets"]}
    check("memcpy size range: average call time",
          close(b[H_CPY[0]]["cpu_only_us"], 4000) and close(b[H_CPY[0]]["with_dsa_us"], 1000),
          b.get(H_CPY[0]))
    check("no notes for a clean run", r["notes"] == [], r["notes"])

# --- the DSA run used no DSA ---
fallback = summary(2, {**cpu_cpy, **cpu_set}, hist(cpu_cpy, cpu_set), stdc_only=1)
r = report(write_run_dir([("cpu-1", "cpu", 10.0, 10.0, [cpu_rec], 0),
                          ("dsa-1", "dsa", 10.0, 10.0, [fallback], 0)]))
check("says when DTO found no work queue",
      r and any("no usable work queue" in n for n in r["notes"]), r and r["notes"])

# --- different amounts of work ---
more = ops(memcpy_dsa=(2000, 2 * B_CPY, 2e9))
r = report(write_run_dir([("cpu-1", "cpu", 10.0, 10.0, [cpu_rec], 0),
                          ("dsa-1", "dsa", 10.0, 10.0,
                           [summary(2, {**more, **dsa_set}, [])], 0)]))
check("says when the runs did different work",
      r and any("different amounts" in n for n in r["notes"]), r and r["notes"])
check("speedup is per byte", r and close(r["ops"]["memcpy"]["speedup"], 4.0),
      r and r["ops"]["memcpy"])

# --- DSA slower for small calls, faster for big ones ---
small_c, small_d = ops(memset_cpu=(1000, B_SET, 1e8)), ops(memset_dsa=(1000, B_SET, 3e8))
r = report(write_run_dir([
    ("cpu-1", "cpu", 10.0, 10.0, [summary(1, {}, hist(cpu_cpy, small_c))], 0),
    ("dsa-1", "dsa", 10.0, 10.0, [summary(2, {}, hist(dsa_cpy, small_d))], 0)]))
check("suggests DTO_MIN_BYTES from where DSA starts winning",
      r and any(f"DTO_MIN_BYTES={H_CPY[0]}" in n for n in r["notes"]), r and r["notes"])

# --- a failed run ---
r = report(write_run_dir([("cpu-1", "cpu", 10.0, 10.0, [cpu_rec], 0),
                          ("dsa-1", "dsa", 1.0, 1.0, [], 3)]))
check("says a run failed", r and any("exited with status 3" in n for n in r["notes"]),
      r and r["notes"])

p = subprocess.run([REPORT, tempfile.mkdtemp()], capture_output=True, text=True)
check("a directory without runs.jsonl is an error", p.returncode != 0, p.stdout)

# --- perf sample categories ---
loader = importlib.machinery.SourceFileLoader("dto_stats_report", REPORT)
spec = importlib.util.spec_from_loader("dto_stats_report", loader)
rep = importlib.util.module_from_spec(spec)
loader.exec_module(rep)
lines = ["400|libc.so.6           |[.] __memmove_avx512_unaligned_erms",
         "100|libc.so.6           |[.] __memset_evex_unaligned_erms",
         "100|libc.so.6           |[.] __memcmp_evex_movbe",
         "100|app                 |[.] crc32c_hw",
         "100|[kernel.kallsyms]   |[k] rep_movs_alternative",
         "50|[kernel.kallsyms]    |[k] clear_page_erms",
         "50|[kernel.kallsyms]    |[k] schedule",
         "50|libdto.so            |[.] dsa_wait_busy_poll",
         "40|app                  |[.] main",
         "10|libc.so.6            |[.] 0x00000000000fffff",
         "not a sample line"]
shares, unknown = rep.classify(lines)
want = {"memcpy/memmove": 0.4, "memset": 0.1, "memcmp": 0.1, "CRC": 0.1,
        "kernel copy/clear": 0.15, "other kernel code": 0.05, "DTO library": 0.05,
        "other user code": 0.04, "libc, unknown function": 0.01}
check("perf samples by category", all(close(shares[k], v) for k, v in want.items()), shares)
check("unplaced libc share", close(unknown, 0.01), unknown)

# --- libc functions by address, in this machine's libc ---
loc = rep.LibcLocator()
if not loc.ok:
    print("SKIP: libc located by address (no build ID or not 64-bit ELF)")
else:
    libc = ctypes.CDLL("libc.so.6")
    base = [int(line.split("-")[0], 16) for line in open("/proc/self/maps")
            if rep.is_libc(line.split()[-1]) and int(line.split()[2], 16) == 0][0]
    for name, cat in (("memmove", "memcpy/memmove"), ("memset", "memset"),
                      ("memcmp", "memcmp")):
        vaddr = ctypes.cast(getattr(libc, name), ctypes.c_void_p).value - base + 16
        # perf reports file offsets
        off = [vaddr - v + o for o, v, n in loc.loads if v <= vaddr < v + n][0]
        check(f"a sample just inside {name} is {cat}", loc.category(off) == cat,
              loc.category(off))

print()
if failures:
    print(f"{failures} stats report test(s) failed")
    sys.exit(1)
print("All stats report tests passed")
