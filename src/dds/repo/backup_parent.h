#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "dds/hash/fingerprint.h"
#include "dds/snapshot/source_tree.h"
#include "dds/snapshot/tree.h"

namespace dds {

struct RepoInternals;
class IngestSession;

// A backup with a parent snapshot (D106) skips reading a file that is unchanged since
// the parent and reuses the parent's recipe pieces for it.

// A file's mtime must be at least this much older than the parent backup's start for the file to
// be skipped. File systems stamp mtimes from a coarse clock (a few milliseconds on Linux, up to 2 s
// on FAT), so a file rewritten at the same size within one tick of the parent reading it keeps the
// same mtime; one whose mtime is older than the parent's start by more than a tick can only have
// changed since with a newer mtime.
inline constexpr int64_t kParentMtimeMarginSec = 2;

// Whether the file listed as `now` may reuse `parent`'s pieces without being read: same kind,
// size, and mtime to the nanosecond, and that mtime more than kParentMtimeMarginSec older than the
// parent backup's start (`parent_started_at_ns`; 0, unknown for a version 1 manifest, never
// matches). A change that keeps the size and restores the old mtime is not noticed (SPEC.md
// section 4).
bool UnchangedSinceParent(const SourceEntry& now, const TreeEntry& parent,
                          int64_t parent_started_at_ns);

// The entry named `name` in `tree` (sorted bytewise by name), or nullptr.
const TreeEntry* FindEntry(const Tree& tree, std::string_view name);

// Reads and decodes tree blob `fp` of the parent, under a read guard. Nullptr, with a warning, if
// it cannot be read (GC may have removed it if the parent was removed meanwhile): the files below
// it are then read as without a parent.
std::unique_ptr<Tree> ReadParentTree(RepoInternals& repo, const Fingerprint& fp);

// Gives `entry` the parent's size and pieces if every blob they reference is still in the
// repository, looking each up through `session`: the recipe pieces, and every data chunk each
// recipe lists. The lookups stamp every one of them with the session's epoch, as a read file's
// chunks are stamped, so GC keeps them even if the parent snapshot is removed while this backup
// runs (ARCHITECTURE.md 6.2 item 3). False if any blob is missing or a recipe cannot be read: the
// caller then reads the file. The caller holds a ReadGuard for the recipe reads (6.3).
bool ReuseParentPieces(RepoInternals& repo, IngestSession& session, const TreeEntry& parent,
                       TreeEntry& entry);

}  // namespace dds
