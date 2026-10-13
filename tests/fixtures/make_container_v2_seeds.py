#!/usr/bin/env python3
"""Writes version 2 container seeds for the container fuzz target from ON_DISK_FORMAT.md section 5
alone (D107): one with two blobs stored as content, one with a blob stored as a zstd frame. Footer
validation reads no payload, so the payload bytes and fingerprints are arbitrary.

    tests/fixtures/make_container_v2_seeds.py fuzz/corpus/container
"""
import os, struct, sys

def crc32c(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF

assert crc32c(b"123456789") == 0xE3069283
REPO = bytes(range(0xA0, 0xB0))

def container(blobs):
    # blobs: (fp tag, type, codec, stored bytes, raw length)
    header = b"DDSCTR01" + struct.pack("<II", 2, 1) + REPO
    body, footer, offset = b"", struct.pack("<I", len(blobs)), 32
    for tag, btype, codec, stored, raw in blobs:
        footer += bytes([tag]) * 32 + struct.pack("<BBIIII", btype, codec, offset, len(stored), raw,
                                                  crc32c(stored))
        body += stored
        offset += len(stored)
    trailer = struct.pack("<IIIII", offset, len(footer), len(blobs), crc32c(footer), crc32c(header))
    trailer += struct.pack("<Q", offset - 32) + b"DDSE"
    return header + body + footer + trailer

out = sys.argv[1]
open(os.path.join(out, "v2-plain"), "wb").write(
    container([(1, 1, 0, b"hello", 5), (2, 2, 0, b"tree bytes", 10)]))
open(os.path.join(out, "v2-zstd"), "wb").write(
    container([(3, 1, 1, b"\x28\xb5\x2f\xfd frame", 4096), (4, 3, 0, b"r", 1)]))
print("wrote", sorted(f for f in os.listdir(out) if f.startswith("v2-")))
