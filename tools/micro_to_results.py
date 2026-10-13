#!/usr/bin/env python3
"""Converts Google Benchmark JSON (BENCHMARKS.md section 6) into result files (section 3).

    micro_to_results.py --micro micro.json --meta META.json --results DIR

One result file per benchmark family (BM_Crc32c, BM_FastCdc, ...), experiment "micro". Each
repetition is one sample; its counters are, per variant (for example "BM_Crc32c/4096"), the real
time per iteration in nanoseconds and the bytes or items per second Google Benchmark reported.
Aggregate rows (mean, median, stddev) are ignored: the summary is recomputed like every other result.
"""
import argparse
import collections
import json
import os
import subprocess
import sys

UNIT_NS = {"ns": 1, "us": 1e3, "ms": 1e6, "s": 1e9}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--micro", required=True)
    p.add_argument("--meta", required=True)
    p.add_argument("--results", required=True)
    args = p.parse_args()
    with open(args.micro) as f:
        micro = json.load(f)
    # family -> repetition -> counters
    families = collections.defaultdict(lambda: collections.defaultdict(dict))
    for b in micro["benchmarks"]:
        if b.get("run_type") != "iteration":
            continue
        name = b["run_name"]
        family = name.split("/")[0]
        rep = b.get("repetition_index", 0)
        counters = families[family][rep]
        counters[name + ".real_time_ns"] = b["real_time"] * UNIT_NS[b["time_unit"]]
        for key in ("bytes_per_second", "items_per_second"):
            if key in b:
                counters[name + "." + key] = b[key]
    here = os.path.dirname(os.path.abspath(__file__))
    for family, reps in sorted(families.items()):
        samples = [{"wall_s": 0, "counters": reps[r]} for r in sorted(reps)]
        driver = {"experiment": "micro", "params": {"family": family,
                                                    "library": "google-benchmark"},
                  "dataset": {"name": "in-memory buffers"}, "samples": samples}
        tmp = args.micro + "." + family + ".driver.json"
        with open(tmp, "w") as f:
            json.dump(driver, f)
        subprocess.run([sys.executable, os.path.join(here, "finalize_result.py"), "--driver", tmp,
                        "--meta", args.meta, "--results", args.results, "--tag", family],
                       check=True)


if __name__ == "__main__":
    main()
