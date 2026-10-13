#!/usr/bin/env bash
# Sourced by every benchmark job (BENCHMARKS.md sections 1-2): the clean toolchain environment,
# a release build with GCC on node-local disk, the machine and build record (meta.json), dataset
# preparation on node-local disk, and `measure`, which runs dds_bench and turns its output into a
# result file under $DDS_OUT/results/<experiment>/.
set -uo pipefail
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/env.sh"
dds_record_machine
DATA="/scratch/$USER/dedupstore-data"
WORK="${TMPDIR:-/tmp}/dds-bench-$SLURM_JOB_ID"
BUILD="$WORK/build"
RESULTS="$DDS_OUT/results"
mkdir -p "$WORK" "$RESULTS"
trap 'rm -rf "$WORK"' EXIT
# Build from a node-local copy of this job's own source directory (src-<commit>, D105): faster
# than the network file system, and unaffected by anything uploaded while the job runs.
cp -a "$DDS_SRC" "$WORK/src" || { echo "cannot copy the source"; exit 1; }
DDS_SRC="$WORK/src"
export DDS_SRC

# The reported compiler (D94): the system GCC 11.5, release preset (-O2 -g -DNDEBUG).
export CC=$DDS_GCC_CC CXX=$DDS_GCC_CXX
(cd "$DDS_SRC" && cmake --preset release -B "$BUILD" -DDDS_DEPS_MIRROR="$DDS_DEPS_MIRROR" \
                 -DDDS_BUILD_TESTS=OFF) > "$DDS_OUT/configure.log" 2>&1 &&
  cmake --build "$BUILD" -j "$SLURM_CPUS_ON_NODE" --target dds dds_bench dds_gen_synth \
    dds_micro_bench > "$DDS_OUT/build.log" 2>&1 || { echo "build failed"; exit 1; }
DDS="$BUILD/dds"
BENCH="$BUILD/bench/dds_bench"
GEN_SYNTH="$BUILD/bench/dds_gen_synth"

# meta.json: what every result file records about the machine and build (section 3).
python3 - "$DDS_OUT/meta.json" "$WORK" <<'PY'
import json, os, platform, subprocess, sys, time
def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout.strip()
info = dict(l.split("=", 1) for l in open(os.environ["DDS_SRC"] + "/dds-build-info.txt").read().split() if "=" in l)
meta = {
    "git_commit": info.get("commit", "unknown"),
    "git_dirty": False,  # jobs build `git archive HEAD` (D57)
    "started_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "machine": {
        "node": platform.node().split(".")[0],
        "cpu": sh("lscpu | sed -n 's/^Model name: *//p'"),
        "cores": int(sh("nproc")),
        "mem_gb": int(sh("free -g | awk '/Mem:/ {print $2}'")),
        "kernel": platform.release(),
        "fs_repo": sh("stat -f -c %T " + sys.argv[2]),
        "fs_dataset": sh("stat -f -c %T " + sys.argv[2]),
        "slurm_job": os.environ.get("SLURM_JOB_ID", ""),
    },
    "build": {
        "compiler": sh(os.environ["CXX"] + " --version | head -1"),
        "preset": "release",
        "flags": "-O2 -g -DNDEBUG",
    },
}
json.dump(meta, open(sys.argv[1], "w"), indent=1)
PY

# extract_kernel V...: node-local copies of the kernel releases (fetched by fetch_kernel.sh).
extract_kernel() {
  mkdir -p "$WORK/kernel"
  for v in "$@"; do
    [ -d "$WORK/kernel/linux-$v" ] || tar -xJf "$DATA/kernel/linux-$v.tar.xz" -C "$WORK/kernel" ||
      { echo "cannot extract linux-$v"; exit 1; }
  done
}
kernel_dirs() { local out=""; for v in "$@"; do out="$out${out:+,}$WORK/kernel/linux-$v"; done; echo "$out"; }

# warm DIR...: read every file once ("warm source cache", BENCHMARKS.md section 1).
warm() { for d in "$@"; do find "$d" -type f -print0 | xargs -0 cat > /dev/null; done; }

# build_side COMMIT TAG: builds dds_bench, next to the job's own build, from src-COMMIT (that
# commit's source, uploaded next to the job's before submission), and writes meta-TAG.json for its
# results (D99).
build_side() {
  local commit="$1" tag="$2" src="$WORK/src-$1" build="$WORK/build-$1"
  cp -a "/scratch/$USER/dedupstore/src-$commit" "$src" || { echo "no src-$commit"; exit 1; }
  grep -q "^commit=$commit" "$src/dds-build-info.txt" ||
    { echo "src-$commit holds another commit"; exit 1; }
  (cd "$src" && cmake --preset release -B "$build" -DDDS_DEPS_MIRROR="$DDS_DEPS_MIRROR" \
                 -DDDS_BUILD_TESTS=OFF) > "$DDS_OUT/configure-$tag.log" 2>&1 &&
    cmake --build "$build" -j "$SLURM_CPUS_ON_NODE" --target dds_bench \
      > "$DDS_OUT/build-$tag.log" 2>&1 || { echo "$tag build failed"; exit 1; }
  python3 - "$DDS_OUT/meta.json" "$DDS_OUT/meta-$tag.json" \
    "$(sed -n 's/^commit=//p' "$src/dds-build-info.txt")" <<'PY'
import json, sys
meta = json.load(open(sys.argv[1]))
meta["git_commit"] = sys.argv[3]
json.dump(meta, open(sys.argv[2], "w"), indent=1)
PY
}

# measure EXPERIMENT TAG dds_bench-args...: run $BENCH, then finalize into $RESULTS with $META
# (default: this job's meta.json).
measure() {
  local experiment="$1" tag="$2"
  shift 2
  local out="$WORK/driver-$experiment-$tag.json"
  echo "=== $experiment $tag: $*"
  if "$BENCH" "$@" --experiment "$experiment" --out "$out" > "$DDS_OUT/$experiment-$tag.log" 2>&1 &&
     python3 "$DDS_SRC/tools/finalize_result.py" --driver "$out" \
       --meta "${META:-$DDS_OUT/meta.json}" \
       --results "$RESULTS" --tag "$tag" >> "$DDS_OUT/$experiment-$tag.log" 2>&1; then
    echo "$experiment $tag: ok"
  else
    echo "$experiment $tag: FAILED (see $experiment-$tag.log)"
    overall=1
  fi
}
overall=0
