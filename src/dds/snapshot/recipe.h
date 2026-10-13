#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dds/common/result.h"
#include "dds/hash/fingerprint.h"

namespace dds {

inline constexpr uint32_t kMaxRefsPerRecipe = 4096;  // D14
inline constexpr uint32_t kRecipeFormatVersion = 1;

// One data chunk of a file, with its length so restore can check sizes before reading.
struct ChunkRef {
  Fingerprint fp;
  uint32_t length = 0;
};

// One piece of a file's chunk list: 1 to 4096 chunk references in file order (ON_DISK_FORMAT.md
// section 7). A file's recipe is the sequence of its pieces, two levels so that no metadata blob
// grows with the file (D14).
struct Recipe {
  std::vector<ChunkRef> refs;
};

std::vector<std::byte> EncodeRecipe(const Recipe& recipe);

// Corruption unless 1..4096 refs, each of length 1..max_chunk_size, and no trailing bytes.
Result<Recipe> DecodeRecipe(std::span<const std::byte> bytes, uint32_t max_chunk_size);

}  // namespace dds
