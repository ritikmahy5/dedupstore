#pragma once

#include <array>
#include <cstdint>

namespace dds {

// The default gear seed: ASCII "dedupstr" read as a big-endian u64 (D8).
inline constexpr uint64_t kDefaultGearSeed = 0x6465647570737472ULL;

using GearTable = std::array<uint64_t, 256>;

// The 256 Gear hash constants for `seed`: entry i is the (i + 1)-th output of SplitMix64 started
// from state `seed` (Vigna, https://prng.di.unimi.it/splitmix64.c). The Gear hash only needs
// constants that look random; SplitMix64 makes them reproducible from one stored number.
GearTable GenerateGearTable(uint64_t seed);

// GenerateGearTable(kDefaultGearSeed), committed as a constant so that the table every default
// repository depends on is visible in review (D8). A test regenerates it.
extern const GearTable kDefaultGearTable;

}  // namespace dds
