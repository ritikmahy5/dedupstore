#!/bin/bash
#SBATCH --job-name=dds-e2-ab
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:30:00
#
# The ChunkReader fix (D98) measured on one node (D99): the first runs (8dd74df, node d0030) and
# the reruns (620f2a6, node d0020) differ in both code and node, and the same configuration varied
# by up to 17% between those jobs, so their throughputs cannot isolate the fix. This job builds the
# commit before the fix next to the job's own commit and runs E2's kernel ingest with each,
# alternating old, new, new, old at 8 KiB and 64 KiB and once each at 1 MiB (the old build takes
# about 15 s per version there). Experiment E2ab; tags name the build, the average, and the pass.
# Needs commit 8dd74df's source uploaded next to the job's (src-8dd74df) before submission.
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
OLD_COMMIT=8dd74df
OLD_SRC="$WORK/src-$OLD_COMMIT"
OLD_BUILD="$WORK/build-$OLD_COMMIT"
cp -a "/scratch/$USER/dedupstore/src-$OLD_COMMIT" "$OLD_SRC" ||
  { echo "no src-$OLD_COMMIT"; exit 1; }
grep -q "^commit=$OLD_COMMIT" "$OLD_SRC/dds-build-info.txt" ||
  { echo "src-$OLD_COMMIT holds another commit"; exit 1; }
(cd "$OLD_SRC" && cmake --preset release -B "$OLD_BUILD" -DDDS_DEPS_MIRROR="$DDS_DEPS_MIRROR" \
                 -DDDS_BUILD_TESTS=OFF) > "$DDS_OUT/configure-old.log" 2>&1 &&
  cmake --build "$OLD_BUILD" -j "$SLURM_CPUS_ON_NODE" --target dds_bench \
    > "$DDS_OUT/build-old.log" 2>&1 || { echo "old build failed"; exit 1; }
NEW_BENCH="$BENCH"
OLD_BENCH="$OLD_BUILD/bench/dds_bench"
# The old build's results must name the old commit.
old_full="$(sed -n 's/^commit=//p' "$OLD_SRC/dds-build-info.txt")"
python3 - "$DDS_OUT/meta.json" "$DDS_OUT/meta-old.json" "$old_full" <<'PY'
import json, sys
meta = json.load(open(sys.argv[1]))
meta["git_commit"] = sys.argv[3]
json.dump(meta, open(sys.argv[2], "w"), indent=1)
PY
KERNEL="6.0 6.1 6.2 6.3 6.4 6.5 6.6 6.7 6.8 6.9 6.10"
extract_kernel $KERNEL
warm "$WORK/kernel"
ab() {  # ab BUILD AVG PASS
  if [ "$1" = old ]; then
    BENCH="$OLD_BENCH" META="$DDS_OUT/meta-old.json"
  else
    BENCH="$NEW_BENCH" META=""
  fi
  measure E2ab "$1-avg-$2-$3" ingest --versions "$(kernel_dirs $KERNEL)" --labels "${KERNEL// /,}" \
    --dataset kernel --storage mem --chunker fastcdc --avg "$2" --threads 16
}
for avg in 8K 64K; do
  ab old $avg a; ab new $avg a; ab new $avg b; ab old $avg b
done
ab old 1M a; ab new 1M a
echo "overall: $overall"
exit $overall
