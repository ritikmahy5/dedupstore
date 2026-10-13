#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dds::testing {

// `size` pseudo-random bytes from std::mt19937_64 seeded with `seed`: the eight bytes of each
// output, least significant first. The standard fixes mt19937_64's output sequence, so the bytes
// are the same on every platform and compiler, which the golden fixtures rely on.
std::vector<std::byte> RandomBytes(uint64_t seed, size_t size);

}  // namespace dds::testing
