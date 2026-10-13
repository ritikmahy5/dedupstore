#pragma once

#include <string>
#include <vector>

#include "dds/snapshot/mem_node.h"

namespace dds::testing {

// Differences between an expected and an actual tree, one readable line each (empty = equal).
// Compares kind, file content, symlink target, permission bits, and mtime to the nanosecond.
// Special entries in `expected` are skipped (backups skip them), and symlink modes are not
// compared (D54: recorded, never restored).
std::vector<std::string> DiffMemTrees(const MemTree& expected, const MemTree& actual);

// A real directory read into a MemTree (through PosixSourceTree), for comparing restores on disk.
MemTree ReadDirectory(const std::string& root);

}  // namespace dds::testing
