#!/bin/bash
#SBATCH --job-name=dds-e4
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:00:00
#
# E4 (BENCHMARKS.md 5): restore throughput and fragmentation. Restore 6.0 and 6.10 from a disk
# repository holding all 11 versions, and 6.10 from one holding only 6.10; with and without BLAKE3
# verification. Repositories and restores on node-local disk.
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
KERNEL="6.0 6.1 6.2 6.3 6.4 6.5 6.6 6.7 6.8 6.9 6.10"
extract_kernel $KERNEL
"$DDS" init "$WORK/all" > /dev/null 2>&1 && "$DDS" init "$WORK/only" > /dev/null 2>&1
for v in $KERNEL; do
  id=$("$DDS" backup "$WORK/all" "$WORK/kernel/linux-$v" 2>> "$DDS_OUT/build-repos.log") || { echo "backup failed"; exit 1; }
  [ "$v" = 6.0 ] && ID_FIRST=$id
  [ "$v" = 6.10 ] && ID_LAST=$id
done
ID_ONLY=$("$DDS" backup "$WORK/only" "$WORK/kernel/linux-6.10" 2>> "$DDS_OUT/build-repos.log") || exit 1
rm -rf "$WORK/kernel"   # restores must read the repository, not a cached source tree
for verify in 1 0; do
  measure E4 "all-6.0-verify$verify" restore --repo "$WORK/all" --snapshot $ID_FIRST --dest "$WORK/dest" --verify $verify --label kernel-6.0-from-all
  measure E4 "all-6.10-verify$verify" restore --repo "$WORK/all" --snapshot $ID_LAST --dest "$WORK/dest" --verify $verify --label kernel-6.10-from-all
  measure E4 "only-6.10-verify$verify" restore --repo "$WORK/only" --snapshot $ID_ONLY --dest "$WORK/dest" --verify $verify --label kernel-6.10-alone
done
echo "overall: $overall"
exit $overall
