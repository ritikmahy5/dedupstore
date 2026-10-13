#!/usr/bin/env python3
"""Turns dds_bench output into a result file (BENCHMARKS.md section 3).

    finalize_result.py --driver OUT.json --meta META.json --results bench/results --tag CONFIG

Adds the run id (UTC timestamp + short commit + node + a tag naming the configuration), the
commit and dirty flag, the machine and build records the SLURM job wrote to META.json, and the
median, min, and max of every counter over the measured samples. Writes
<results>/<experiment>/<run_id>.json. Existing files are never replaced (BENCHMARKS.md section 2):
if the name is taken, it fails.

Standard library only, so it runs on Explorer's system Python 3.9.
"""
import argparse
import json
import os
import statistics
import sys
import time


def summarize(samples):
    keys = sorted({k for s in samples for k in s["counters"]})
    summary = {"median": {}, "min": {}, "max": {}}
    for key in keys + ["wall_s"]:
        values = [s["counters"][key] if key != "wall_s" else s["wall_s"]
                  for s in samples if key == "wall_s" or key in s["counters"]]
        if not values:
            continue
        summary["median"][key] = statistics.median(values)
        summary["min"][key] = min(values)
        summary["max"][key] = max(values)
    return summary


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--driver", required=True)
    p.add_argument("--meta", required=True)
    p.add_argument("--results", required=True)
    p.add_argument("--tag", required=True)
    args = p.parse_args()
    with open(args.driver) as f:
        driver = json.load(f)
    with open(args.meta) as f:
        meta = json.load(f)
    if not driver.get("samples"):
        sys.exit("finalize_result: no samples in " + args.driver)
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    run_id = "%s-%s-%s-%s" % (stamp, meta["git_commit"][:7], meta["machine"]["node"], args.tag)
    result = {
        "schema": 1,
        "run_id": run_id,
        "experiment": driver["experiment"],
        "git_commit": meta["git_commit"],
        "git_dirty": meta.get("git_dirty", False),
        "started_at": meta.get("started_at", stamp),
        "machine": meta["machine"],
        "build": meta["build"],
        "dataset": driver.get("dataset", {}),
        "params": driver.get("params", {}),
        "samples": driver["samples"],
        "summary": summarize(driver["samples"]),
    }
    directory = os.path.join(args.results, driver["experiment"])
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, run_id + ".json")
    # O_EXCL: append-only results; a name collision is an error, never an overwrite.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    with os.fdopen(fd, "w") as f:
        json.dump(result, f, indent=1, sort_keys=True)
        f.write("\n")
    print(path)


if __name__ == "__main__":
    main()
