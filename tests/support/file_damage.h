#pragma once

#include <gtest/gtest.h>

#include <string>

#include "dds/fs/mem_fs.h"

namespace dds::testing {

// Rewrites a MemFs file with one bit flipped, as a media error would leave it. The file is replaced
// rather than changed in place (MemFs files are immutable once closed), so readers holding the old
// handle still see the old bytes: reopen the repository to see the damage.
inline void FlipBit(MemFs& fs, const std::string& path, size_t byte, int bit) {
  auto data = ReadWholeFile(fs, path);
  ASSERT_TRUE(data.ok()) << data.status().ToString();
  ASSERT_LT(byte, data->size());
  (*data)[byte] ^= static_cast<std::byte>(1 << bit);
  ASSERT_TRUE(fs.Remove(path).ok());
  auto f = fs.CreateNew(path);
  ASSERT_TRUE(f.ok());
  ASSERT_TRUE((*f)->Append(*data).ok());
  ASSERT_TRUE((*f)->Close().ok());
}

}  // namespace dds::testing
