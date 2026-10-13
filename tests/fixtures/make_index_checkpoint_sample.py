#!/usr/bin/env python3
"""Writes index checkpoint files from ON_DISK_FORMAT.md section 9 alone, independently of the C++
encoder (D104): the codec test's sample (tests/fixtures/index_checkpoint_sample.bin, which the test
requires to equal its own encoding byte for byte) and the fuzz seeds in fuzz/corpus/checkpoint/.

    tests/fixtures/make_index_checkpoint_sample.py tests/fixtures fuzz/corpus/checkpoint
"""
import struct, sys, os
def crc32c(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF
assert crc32c(b"123456789") == 0xE3069283
REPO = bytes(range(0xA0, 0xB0))
def container(cid, n, payload):
    return (cid, payload, 32 + payload + 4 + 50 * n + 32, n)  # a version 2 container
def build(entries, containers, next_id):
    # entries: (fp bytes32, type, container, offset, length, crc), all stored as content (codec 0)
    shards = [[] for _ in range(256)]
    for e in entries: shards[e[0][0]].append(e)
    body = b""; table = b""
    for s in shards:
        s.sort(key=lambda e: e[0])
        data = b"".join(fp + struct.pack("<BBIIII", t, 0, c, o, l, k) for fp, t, c, o, l, k in s)
        body += data
        table += struct.pack("<II", len(s), crc32c(data))
    ctable = b"".join(struct.pack("<IQQI", *c) for c in containers)
    tables = ctable + table
    trailer = b"DDSIDX01" + struct.pack("<I", 2) + REPO + struct.pack("<IIQI", next_id, len(containers), len(entries), crc32c(tables))
    trailer += struct.pack("<I", crc32c(trailer))
    return body + tables + trailer
fixtures, out = sys.argv[1], sys.argv[2]
def fp(shard, tag): return bytes([shard]) + bytes([tag]) * 31
open(os.path.join(out, "empty"), "wb").write(build([], [], 1))
# The codec test's Sample(): CRC32C fields are 0x12345678 + the fingerprint tag.
def entry(shard, tag, t, c, o, l): return (fp(shard, tag), t, c, o, l, 0x12345678 + tag)
two = build([entry(0, 1, 1, 3, 32, 100), entry(7, 2, 2, 3, 132, 50), entry(7, 1, 3, 9, 32, 10),
             entry(255, 4, 1, 12, 32, 4096)],
            [container(3, 2, 150), container(9, 1, 10), container(12, 1, 4096)], 1000)
open(os.path.join(out, "three-containers"), "wb").write(two)
open(os.path.join(out, "truncated"), "wb").write(two[:-1])
open(os.path.join(fixtures, "index_checkpoint_sample.bin"), "wb").write(two)
print("wrote", sorted(os.listdir(out)))
