#include "dds/chunk/gear_table.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace dds {
namespace {

// Outputs of the reference implementation (Vigna, https://prng.di.unimi.it/splitmix64.c),
// compiled and run unmodified on 2026-10-07. They pin GenerateGearTable to the published
// SplitMix64, independently of this project's code.
TEST(GearTableTest, MatchesReferenceSplitMix64ForSeedZero) {
  const auto table = GenerateGearTable(0);
  EXPECT_EQ(table[0], 0xe220a8397b1dcdafULL);
  EXPECT_EQ(table[1], 0x6e789e6aa1b965f4ULL);
  EXPECT_EQ(table[2], 0x06c45d188009454fULL);
  EXPECT_EQ(table[3], 0xf88bb8a8724c81ecULL);
}

TEST(GearTableTest, MatchesReferenceSplitMix64ForDefaultSeed) {
  const auto table = GenerateGearTable(kDefaultGearSeed);
  EXPECT_EQ(table[0], 0x033748d8a17ca6ffULL);
  EXPECT_EQ(table[1], 0x0f993136ef8b9c4eULL);
  EXPECT_EQ(table[2], 0x20f26e007e2625bfULL);
  EXPECT_EQ(table[3], 0x6c5440e3bda0dc9bULL);
  EXPECT_EQ(table[255], 0x1aa93463c967ade7ULL);
}

TEST(GearTableTest, CommittedTableEqualsRegeneratedTable) {
  // D8: the default table is committed as a constant; regenerating it must give the same values,
  // or every existing repository's chunk boundaries (and dedup) would change.
  EXPECT_EQ(GenerateGearTable(kDefaultGearSeed), kDefaultGearTable);
}

TEST(GearTableTest, DefaultSeedSpellsDedupstr) {
  std::string spelled;
  for (int shift = 56; shift >= 0; shift -= 8) {
    spelled.push_back(static_cast<char>((kDefaultGearSeed >> shift) & 0xFF));
  }
  EXPECT_EQ(spelled, "dedupstr");
}

TEST(GearTableTest, DifferentSeedsGiveDifferentTables) {
  EXPECT_NE(GenerateGearTable(1), GenerateGearTable(2));
}

}  // namespace
}  // namespace dds
