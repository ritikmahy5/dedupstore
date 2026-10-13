#pragma once

#include <cstdint>

namespace dds {

// One SplitMix64 step (Vigna, https://prng.di.unimi.it/splitmix64.c, transcribed unchanged):
// advance the state by the golden-ratio increment, then mix it with two xor-shift-multiply
// rounds. The constants are from the reference implementation. Used where library code needs
// reproducible pseudo-random numbers from a seed: the gear table (D62) and CrashSimFs (D76).
// gear_table_test.cc checks it against outputs of the reference program.
inline uint64_t SplitMix64Next(uint64_t& state) {
  uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

}  // namespace dds
