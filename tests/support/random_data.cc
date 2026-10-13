#include "support/random_data.h"

#include <random>

namespace dds::testing {

std::vector<std::byte> RandomBytes(uint64_t seed, size_t size) {
  std::mt19937_64 rng(seed);
  std::vector<std::byte> out(size);
  for (size_t i = 0; i < size; i += 8) {
    uint64_t v = rng();
    for (size_t j = i; j < i + 8 && j < size; ++j, v >>= 8) out[j] = static_cast<std::byte>(v);
  }
  return out;
}

}  // namespace dds::testing
