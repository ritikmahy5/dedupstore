#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dds::testing {

// Seeds for randomized tests (TESTING.md section 1). If the environment variable DDS_TEST_SEED
// is set, every randomized test uses exactly that seed, so a failure can be replayed. Otherwise
// tests use the fixed list below. Tests print the seed in their failure messages with
// SeedMessage().
std::vector<uint64_t> TestSeeds(size_t default_count);

// The first seed of TestSeeds(1).
uint64_t TestSeed();

// "seed=<n> (replay with DDS_TEST_SEED=<n>)", for SCOPED_TRACE and assertion messages.
std::string SeedMessage(uint64_t seed);

}  // namespace dds::testing
