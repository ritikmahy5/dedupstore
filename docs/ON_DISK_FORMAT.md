# dedupstore — on-disk format (version 1)

Every byte the repository stores. A decoder must reject anything this document does not allow,
with `Status::Corruption` naming the file and offset. It must never crash or read out of bounds
(G4; TESTING.md fuzzes every decoder).

## 1. Conventions

- Integers are little-endian, fixed width, unaligned, encoded only through
  `src/dds/common/encoding.h` (D27).
- `u8/u16/u32/u64` are unsigned; `i64` is two's complement.
- `bytes[n]` is raw bytes. `str16` is a `u16` length followed by that many bytes (no terminator).
- `fp` is a 32-byte fingerprint: `BLAKE3-256(tag ‖ payload)` with tag `0x01` data, `0x02` tree,
  `0x03` recipe (D7).
- CRC32C is the Castagnoli CRC (polynomial `0x1EDC6F41`, reflected `0x82F63B78`, initial value and
  final XOR `0xFFFFFFFF`). Check value: `CRC32C("123456789") = 0xE3069283` (D26).
- Every structure has a version. A reader that meets a version it does not know returns
  `Incompatible`, not `Corruption`.

## 2. Repository directory

```
config                         text, section 3
lock                           text, section 4 (present only while a process holds the repo)
containers/<id>.ctr            id = 8 lowercase hex digits of the u32 container id, section 5
snapshots/<sid>.snap           sid = 32 lowercase hex digits, section 8
index.ckpt                     index checkpoint, section 9 (optional, derived; open may remove it)
tmp/                           temporary files; any content is deleted on open
```

Any other file in these directories is ignored by `open` and reported by `verify`.

## 3. `config`

UTF-8 text, `\n` line endings, keys in exactly this order, no blank lines, no spaces around `=`:

```
dedupstore-config=2
repo_id=<32 hex>                  16 random bytes from the OS (getentropy) at init
created_at_ns=<decimal i64>
hash=blake3-256
chunker=<fastcdc-2016|fixed>
min_size=<decimal>                fastcdc: 64 ≤ min_size < avg_size; fixed: = avg_size
avg_size=<decimal>                power of two, 256 to 4194304
max_size=<decimal>                fastcdc: avg_size < max_size ≤ 16777216; fixed: = avg_size
normalization=<0..3>              FastCDC normalization level (D8); 0 for fixed
gear_seed=<16 hex>                seed of the gear table generator (D8)
container_target=<decimal>        bytes; 1048576 to 268435456
compression=<none|zstd:LEVEL>     how new blobs are stored (D107); LEVEL 1 to 19, decimal
crc32c=<8 hex>
```

`crc32c` covers every byte before the `crc32c=` line. Version 1 (before D107) is the same without
the `compression` line and means `compression=none`; it is still read, and a decoded version 1
config encodes back to the same bytes.

Canonical form (the encoder writes exactly this, and the decoder accepts nothing else, so a decode
followed by an encode reproduces the bytes): decimals without sign or leading zeros (`0` is
written as `0`); hex in lowercase, at the stated width; every line, including the last, ends with
one `\n`; no other whitespace; no byte-order mark.

Unknown keys, a missing key, keys out of order, non-canonical values (`zstd` without its level, a
level with a leading zero), or values outside the ranges above → `Corruption`. A
`dedupstore-config` value other than 1 or 2 → `Incompatible`. Written once by
`init` (ARCHITECTURE.md 7.7) and never changed.

## 4. `lock`

UTF-8 text:

```
host=<hostname>
pid=<decimal>
pid_start=<decimal>               process start time (below)
acquired_at_ns=<decimal>
command=<the dds command line, truncated to 512 bytes>
```

`pid_start` distinguishes a live process from a later process that reused its PID. On Linux it is
field 22 (`starttime`, clock ticks since boot) of `/proc/<pid>/stat`. On macOS it is
`kp_proc.p_starttime` from `sysctl({CTL_KERN, KERN_PROC, KERN_PROC_PID, pid})`, written as
microseconds since the Unix epoch. Values are only ever compared on the host that wrote them.

Acquisition writes this record to `tmp/lock.<host>.<pid>.<nanos>`, `Sync`s it, then `Link`s it to
`lock`, so `lock` never exists without its full contents. Takeover of a stale lock follows D16. A
lock that does not parse is treated as held by an unknown holder: never taken over automatically.

## 5. Container (`containers/<id>.ctr`)

```
offset 0       header      32 bytes
offset 32      blob region payloads as stored, back to back, no gaps, no per-blob framing
footer_offset  footer      4 + 50 × entry_count bytes (45 in version 1)
file end − 32  trailer     32 bytes
```

Version 2 (D107) adds each blob's codec and content length to its footer entry, so a blob can be
stored as a zstd frame. New containers are written as version 2; version 1 containers, which
store every blob as its content, are still read.

### 5.1 Header (32 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | bytes[8] | magic `DDSCTR01` (ASCII) |
| 8 | u32 | format version: 2 (1 is still read) |
| 12 | u32 | container id (must equal the id in the file name) |
| 16 | bytes[16] | repo_id (must equal `config`) |

The header has no CRC field of its own. The trailer's header CRC (5.4) covers it, so the header
is checked together with the footer.

### 5.2 Blob region

The payloads as stored, in the order they were added. Entry `i` starts where entry `i − 1` ends.
The first starts at 32. The last ends at `footer_offset`. A payload stored with codec 1 is one zstd
frame whose header records its content size; the writer stores a frame only when it is smaller than
the content, and otherwise stores the content (codec 0).

### 5.3 Footer

| Type | Field |
|---|---|
| u32 | entry_count (≥ 1; an empty container is never sealed) |
| entry_count × 50 bytes | entries, in blob-region order (45 bytes each in version 1) |

Entry, version 2 (50 bytes):

| Offset | Type | Field |
|---|---|---|
| 0 | fp | fingerprint (of the content, as always) |
| 32 | u8 | blob type: 1 data, 2 tree, 3 recipe |
| 33 | u8 | codec: 0 stored as content, 1 one zstd frame (D107) |
| 34 | u32 | offset of the stored payload in the file |
| 38 | u32 | stored length (≥ 1) |
| 42 | u32 | content length (1 to the limit: data ≤ config `max_size`; tree and recipe ≤ 64 MiB); equal to the stored length for codec 0, greater for codec 1 |
| 46 | u32 | CRC32C of the stored payload |

Entry, version 1 (45 bytes): fingerprint (0), type (32), offset (33), length (37), CRC32C (41); the
blob is stored as its content, so its stored and content lengths are both `length`.

### 5.4 Trailer (last 32 bytes of the file)

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | footer_offset |
| 4 | u32 | footer_length (= 4 + 50 × entry_count; 4 + 45 × entry_count in version 1) |
| 8 | u32 | entry_count (= the footer's) |
| 12 | u32 | CRC32C of the footer bytes |
| 16 | u32 | CRC32C of the 32 header bytes |
| 20 | u64 | payload_bytes (sum of entry lengths = footer_offset − 32) |
| 28 | bytes[4] | magic `DDSE` |

The trailer itself is protected because every field is cross-checked:
`footer_offset + footer_length + 32 = file size`, `payload_bytes = footer_offset − 32`, the
entry_count fields agree, and both CRCs match.

### 5.5 Validation on open (footer read)

In order, stopping at the first violation:
1. File size ≥ 32 + 1 + 4 + 45 + 32 (`Corruption`).
2. Trailer magic, the size identities above (the footer length must fit 45 or 50 bytes per entry),
   the header CRC, then the header fields: a format version other than 1 or 2 → `Incompatible`; a
   wrong magic, an id that differs from the file name, or a repo_id that differs from `config` →
   `Corruption`; a footer length that does not match the version's entry size → `Corruption`.
   Then the footer CRC (`Corruption`).
3. Entries are contiguous from offset 32 to `footer_offset` (by stored length). Types and codecs
   are valid. Content lengths respect their limits. A codec 0 entry's content length equals its
   stored length; a codec 1 entry's stored length is smaller than its content length.
4. A fingerprint appearing twice in one container is `Corruption` (a writer never does that).

A container file is created only by sealing (ARCHITECTURE.md 7.2), and sealing never replaces an
existing file.

Payload CRCs (of the bytes as stored) are checked on every read of a blob, not on open. A zstd
frame must be exactly the stored bytes, record a content size of 1 to the blob's limit, and decode
to that many bytes, or the read is `Corruption`.

## 6. Tree blob (type 2)

| Type | Field |
|---|---|
| u8 | format version = 1 |
| u32 | entry_count |
| entries | strictly ascending by name, compared as unsigned bytes |

Entry:

| Type | Field |
|---|---|
| str16 | name: 1–255 bytes, no `/`, no NUL, not `.` or `..` |
| u8 | kind: 1 file, 2 directory, 3 symlink |
| u32 | mode: permission bits only (`mode & 07777 == mode`) |
| i64 | mtime seconds |
| u32 | mtime nanoseconds (< 1,000,000,000) |
| kind 1 | u64 size; u32 piece_count (≤ 65,536); piece_count × fp (recipe pieces in file order) |
| kind 2 | fp of the subdirectory's tree blob |
| kind 3 | str16 target: 1–4095 bytes, no NUL |

A file with size 0 has piece_count 0. On restore, the sum of all chunk lengths across a file's
pieces must equal `size`, otherwise `Corruption`.

## 7. Recipe blob (type 3)

| Type | Field |
|---|---|
| u8 | format version = 1 |
| u32 | ref_count (1–4096) |
| ref_count × (fp, u32 length) | data chunks in file order; each length 1..config `max_size` |

The lengths let restore check sizes before reading, and let a future version seek within a file
without reading its chunks.

## 8. Snapshot manifest (`snapshots/<sid>.snap`)

| Type | Field |
|---|---|
| bytes[8] | magic `DDSSNAP1` |
| u32 | format version: 2 (1 is still read) |
| bytes[16] | origin repo_id (the repository that created the snapshot; kept by replication) |
| i64 | created_at_ns: wall clock when the manifest was built, at the end of the backup |
| i64 | started_at_ns, version 2 only: wall clock when the backup started listing (D106) |
| fp | root tree fingerprint |
| u64 | logical_bytes |
| u64 | file_count |
| u64 | directory_count |
| u64 | symlink_count |
| str16 | host (≤ 255 bytes) |
| str16 | source path (≤ 4095 bytes) |
| str16 | tag (≤ 1024 bytes) |
| u32 | CRC32C of every preceding byte |

Version 1 is version 2 without `started_at_ns`. A version 1 manifest decodes, and its decoded
fields encode back to the same bytes, so its `sid` never changes.

`sid` = the first 16 bytes of BLAKE3-256 of the whole file (CRC included), as 32 hex digits. The
file name must equal the computed `sid`, otherwise `Corruption`. Replication copies manifests byte
for byte, so a snapshot has the same id in every repository.

## 9. Index checkpoint (`index.ckpt`)

A copy of the in-memory index, so open can take most entries from one file instead of reading every
container's footer (D104). It is derived, never the source of truth: open checks it against the
list of containers, reads the footers of the containers it does not cover, and removes it when any
check below fails. A repository without one is valid.

```
offset 0             entries          50 × entry_count bytes, by index shard 0..255 (fingerprint
                                      byte 0), fingerprints strictly increasing within a shard
50 × entry_count     container table  24 × container_count bytes, ids strictly increasing
then                 shard table      256 × 8 bytes
file end − 52        trailer          52 bytes
```

Like a container, the tables and trailer come last, so the writer only appends.

### 9.1 Entry (50 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | fp | fingerprint |
| 32 | u8 | blob type: 1 data, 2 tree, 3 recipe |
| 33 | u8 | codec, as in the container footer (5.3) |
| 34 | u32 | container id |
| 38 | u32 | offset of the stored payload in the container file |
| 42 | u32 | stored length |
| 46 | u32 | CRC32C of the stored payload |

Unlike a footer entry (5.3), the entry names its container, since the checkpoint covers many.

### 9.2 Container table and shard table

Container table entry (24 bytes), the covered container's own trailer values (5.4), with which
open marks it published without reading it:

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | container id |
| 4 | u64 | payload_bytes |
| 12 | u64 | file size |
| 20 | u32 | entry_count |

The covered container's file size must fit its version's footer: 45 or 50 bytes per entry.

Shard table entry `i` (8 bytes): u32 entry count of shard `i`, then u32 CRC32C of shard `i`'s
entry bytes. Shard `i`'s entries start at 50 × (the sum of the counts of shards 0 to `i` − 1).

### 9.3 Trailer (last 52 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | bytes[8] | magic `DDSIDX01` (ASCII) |
| 8 | u32 | format version = 2 (version 1, before D107, had no codec byte and is not read) |
| 12 | bytes[16] | repo_id (must equal `config`) |
| 28 | u32 | next_container_id: the writer's next container id (≥ 1) |
| 32 | u32 | container_count |
| 36 | u64 | entry_count |
| 44 | u32 | CRC32C of the container table and shard table (contiguous) |
| 48 | u32 | CRC32C of trailer bytes 0 to 47 |

Every byte is under a CRC: the entries under their shard's, the tables under the tables CRC, and the
trailer under its own.

### 9.4 Validation

In order, stopping at the first violation, every one `Corruption` except the version:
1. File size ≥ 2048 + 52. The trailer CRC, the magic, then the version (other than 2 →
   `Incompatible`, so open removes a version 1 checkpoint and reads the footers), then the repo_id.
2. 50 × entry_count + 24 × container_count + 2048 + 52 = file size, and next_container_id ≥ 1.
3. The tables CRC. Container ids strictly increase and are below next_container_id. Each covered
   container's sizes satisfy its trailer's identities: file size = 32 + payload_bytes + 4 + E ×
   entry_count + 32 with E 45 or 50, entry_count ≥ 1, payload_bytes ≥ entry_count, file size < 4
   GiB. The shard counts add up to entry_count.
4. For each shard, when its entries are loaded: its CRC, then for each entry, fingerprint byte 0
   equals the shard, fingerprints strictly increase, the type and codec are valid, the container is
   in the table, the stored length is within the content limits of 5.5 step 3, and offset ≥ 32 and
   offset + length ≤ 32 + that container's payload_bytes.

### 9.5 How open uses it (D104)

- A covered container that is listed in `containers/` is trusted without reading it: container
  files are immutable, and container ids are never reused (next rule). A covered container that is
  no longer listed (GC deleted it) contributes nothing.
- Open sets the next container id to at least next_container_id, also when it does not load the
  entries, so no id the checkpoint covers is ever given to a new container.
- A checkpoint that fails any check is removed, the repository directory is synced, and the index
  is rebuilt from every footer.

## 10. Limits recap

| Item | Limit |
|---|---|
| Container | < 4 GiB (u32 offsets); sealed at about `container_target` |
| Blob | data ≤ `max_size` (≤ 16 MiB); tree and recipe ≤ 64 MiB |
| Name | 1–255 bytes |
| Symlink target | 1–4095 bytes |
| Recipe piece | ≤ 4096 refs |
| File | ≤ 65,536 pieces |

## 11. Compatibility rule

Any change to this document's byte layouts increments the version of the structure it changes, and
adds a decoder test for the old version or a recorded decision that the old version is
unsupported. At version 2 with version 1 still decoded: the snapshot manifest (D106,
`ManifestTest.Version1GoldenBytesRoundTripUnchanged`), the container
(D107, `ContainerFormatTest.Version1GoldenBytesStillDecode`), and `config` (D107,
`RepoConfigTest.Version1StillDecodesAndRoundTrips`). The index checkpoint is at version 2 and its
version 1 is unsupported: it is derived, so open removes it and reads the footers (D107). Every
other structure is at version 1.
