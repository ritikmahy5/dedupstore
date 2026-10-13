#pragma once

#include <string_view>
#include <vector>

#include "dds/common/encoding.h"
#include "dds/snapshot/source_tree.h"
#include "support/random_data.h"

namespace dds::testing {

inline std::vector<std::byte> Text(std::string_view s) {
  return {AsBytes(s).begin(), AsBytes(s).end()};
}

// The backup and restore edge cases in one tree: empty files, files of exactly min, max, and
// max + 1 bytes at the default chunker parameters, a multi-MiB file, empty and nested directories,
// symlinks (one dangling), non-ASCII and non-UTF-8 names, a pre-1970 mtime, and a FIFO that backup
// must skip. 8 regular files, 4 directories below the root, 2 symlinks.
inline MemSourceTree EdgeCaseTree() {
  MemSourceTree t;
  t.AddDir("empty-dir", 0700, 10, 1);
  t.AddDir("d", 0751, 11, 2);
  t.AddFile("d/empty", {}, 0600, 12, 3);
  t.AddFile("d/min", RandomBytes(1, 2048), 0644, 13, 4);   // exactly min_size
  t.AddFile("d/max", RandomBytes(2, 65536), 0644, 14, 5);  // exactly max_size
  t.AddFile("d/max-plus-one", RandomBytes(3, 65537), 0640, 15, 6);
  t.AddFile("d/big", RandomBytes(4, 3 << 20), 0755, 16, 999999999);
  t.AddDir("d/deep", 0700, 17, 0);
  t.AddDir("d/deep/er", 0700, 18, 0);
  t.AddFile("d/deep/er/f", Text("deep"), 0400, 19, 0);
  t.AddSymlink("link", "d/deep/er/f", 20, 7);
  t.AddSymlink("dangling", "/nowhere/at/all", 21, 8);
  t.AddFile("caf\xc3\xa9 \xe2\x82\xac", Text("non-ascii"), 0644, -100, 0);  // and a pre-1970 mtime
  t.AddFile("\x01\xff raw bytes", Text("bytes"), 0644, 22, 0);
  t.AddSpecial("fifo");
  return t;
}

}  // namespace dds::testing
