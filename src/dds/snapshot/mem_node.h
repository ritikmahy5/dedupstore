#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "dds/snapshot/tree.h"

namespace dds {

// One entry of an in-memory directory tree, shared by MemSourceTree (input to a backup) and
// MemRestoreSink (output of a restore), so tests can compare the two directly.
struct MemNode {
  EntryKind kind = EntryKind::kFile;
  bool special = false;  // a device, FIFO, or socket: listed but never backed up
  uint32_t mode = 0;
  int64_t mtime_sec = 0;
  uint32_t mtime_nsec = 0;
  std::vector<std::byte> content;  // files
  std::string target;              // symlinks

  friend bool operator==(const MemNode&, const MemNode&) = default;
};

// Relative path ("" is the root, components joined with '/') -> node. The root itself is implicit.
using MemTree = std::map<std::string, MemNode>;

// "" + name -> name; "a" + name -> "a/name".
std::string ChildPath(const std::string& parent, std::string_view name);

}  // namespace dds
