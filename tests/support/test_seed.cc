#include "support/test_seed.h"

#include <cstdlib>
#include <string>

namespace dds::testing {

namespace {

// Arbitrary fixed seeds. Their values do not matter; that they never change does.
constexpr uint64_t kFixedSeeds[] = {0x9e3779b97f4a7c15ULL,
                                    1,
                                    2,
                                    3,
                                    42,
                                    0xdeadbeefULL,
                                    0x0123456789abcdefULL,
                                    7777,
                                    0xfeedfacecafebeefULL,
                                    31337,
                                    271828,
                                    314159,
                                    0x5eed5eedULL,
                                    99991,
                                    65537,
                                    1u << 20,
                                    0x6465647570737472ULL,
                                    123456789,
                                    987654321,
                                    2026};

}  // namespace

std::vector<uint64_t> TestSeeds(size_t default_count) {
  if (const char* env = std::getenv("DDS_TEST_SEED"); env != nullptr && *env != '\0') {
    return {std::strtoull(env, nullptr, 0)};
  }
  std::vector<uint64_t> seeds;
  const size_t fixed_count = sizeof(kFixedSeeds) / sizeof(kFixedSeeds[0]);
  for (size_t i = 0; i < default_count; ++i) {
    // Beyond the fixed list, derive more seeds deterministically from the index.
    seeds.push_back(i < fixed_count ? kFixedSeeds[i] : kFixedSeeds[i % fixed_count] + i);
  }
  return seeds;
}

uint64_t TestSeed() { return TestSeeds(1).front(); }

std::string SeedMessage(uint64_t seed) {
  return "seed=" + std::to_string(seed) + " (replay with DDS_TEST_SEED=" + std::to_string(seed) +
         ")";
}

}  // namespace dds::testing
