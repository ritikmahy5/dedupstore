#pragma once

#include <cstddef>
#include <random>
#include <span>

#include "dds/chunk/chunk_reader.h"

namespace dds::testing {

// A ByteSource over a span that returns a random number of bytes (1 to 64 KiB) per Read, to show
// that chunk boundaries do not depend on how input arrives (TESTING.md 3.4).
class RandomSplitByteSource final : public ByteSource {
 public:
  RandomSplitByteSource(std::span<const std::byte> data, std::mt19937_64& rng)
      : data_(data), rng_(rng) {}

  Result<size_t> Read(std::span<std::byte> out) override {
    const size_t want = 1 + static_cast<size_t>(rng_() % 65536);
    const size_t n = std::min({want, out.size(), data_.size() - pos_});
    std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(pos_), n, out.begin());
    pos_ += n;
    return n;
  }

 private:
  std::span<const std::byte> data_;
  std::mt19937_64& rng_;
  size_t pos_ = 0;
};

}  // namespace dds::testing
