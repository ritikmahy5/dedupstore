#!/usr/bin/env bash
# Downloads Linux source releases for the `kernel` dataset (BENCHMARKS.md section 4) into
# $DDS_DATA/kernel (default /scratch/$USER/dedupstore-data/kernel). Network only, no extraction:
# on Explorer it runs on a login node, because compute nodes cannot always reach the internet
# (D57); jobs extract the tarballs themselves.
#
# Each tarball is checked against kernel.org's sha256sums.asc for that directory when the file is
# listed there. The .asc file's PGP signature is not checked (no keyring is set up), so the list
# is trusted as downloaded over HTTPS. A tarball the list does not cover is recorded on first
# download and checked against that record afterwards ("tofu"). verification.txt says which.
#
#   bench/datasets/fetch_kernel.sh 6.9 6.10
set -euo pipefail
data="${DDS_DATA:-/scratch/$USER/dedupstore-data}/kernel"
base="https://cdn.kernel.org/pub/linux/kernel/v6.x"
mkdir -p "$data"
if curl -sSL --fail -o "$data/sha256sums.asc.part" "$base/sha256sums.asc"; then
  mv "$data/sha256sums.asc.part" "$data/sha256sums.asc"
else
  rm -f "$data/sha256sums.asc.part"
  echo "note: no sha256sums.asc at $base" >&2
fi
for version in "$@"; do
  file="linux-$version.tar.xz"
  path="$data/$file"
  if [ ! -f "$path" ]; then
    curl -sSL --fail -o "$path.part" "$base/$file"
    mv "$path.part" "$path"
  fi
  expected=""
  [ -f "$data/sha256sums.asc" ] && expected="$(awk -v f="$file" '$2 == f { print $1 }' "$data/sha256sums.asc")"
  if [ -n "$expected" ]; then
    echo "$expected  $path" | sha256sum -c --quiet -
    method="sha256sums.asc"
  else
    if [ ! -f "$path.sha256" ]; then sha256sum "$path" | awk '{ print $1 }' > "$path.sha256"; fi
    echo "$(cat "$path.sha256")  $path" | sha256sum -c --quiet -
    method="tofu"
  fi
  grep -v "^$file " "$data/verification.txt" 2>/dev/null > "$data/verification.txt.new" || true
  echo "$file $method $(sha256sum "$path" | awk '{ print $1 }')" >> "$data/verification.txt.new"
  mv "$data/verification.txt.new" "$data/verification.txt"
  echo "ok $file ($method)"
done
