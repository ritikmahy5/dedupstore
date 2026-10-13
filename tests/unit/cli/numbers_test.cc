#include "dds/cli/numbers.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace dds::cli {
namespace {

TEST(NumbersTest, ParseSizeAcceptsPlainNumbersAndPowerOfTwoSuffixes) {
  EXPECT_EQ(ParseSize("0"), 0u);
  EXPECT_EQ(ParseSize("2048"), 2048u);
  EXPECT_EQ(ParseSize("8K"), 8192u);
  EXPECT_EQ(ParseSize("64K"), 65536u);
  EXPECT_EQ(ParseSize("4M"), 4194304u);
  EXPECT_EQ(ParseSize("16M"), 16777216u);
}

TEST(NumbersTest, ParseSizeRejectsEverythingElse) {
  for (const char* bad : {"", "K", "M", "8KB", "8k", "8m", "-1", "+8", "1.5K", " 8K", "8K ", "0x10",
                          "18446744073709551616", "18014398509481984K"}) {
    EXPECT_EQ(ParseSize(bad), std::nullopt) << "'" << bad << "'";
  }
}

TEST(NumbersTest, ParseCount) {
  EXPECT_EQ(ParseCount("1"), 1u);
  EXPECT_EQ(ParseCount("32"), 32u);
  EXPECT_EQ(ParseCount("0"), std::nullopt);  // counts are at least 1
  EXPECT_EQ(ParseCount("-3"), std::nullopt);
  EXPECT_EQ(ParseCount("4x"), std::nullopt);
}

TEST(NumbersTest, ParseThreshold) {
  EXPECT_EQ(cli::ParseThreshold("0.5"), 0.5);
  EXPECT_EQ(cli::ParseThreshold("1"), 1.0);
  EXPECT_EQ(cli::ParseThreshold("1.0"), 1.0);
  EXPECT_EQ(cli::ParseThreshold(".9"), 0.9);
  EXPECT_EQ(cli::ParseThreshold("0.001"), 0.001);
  for (const char* bad : {"", "0", "0.0", "1.01", "2", "-0.5", "+0.5", "1e-1", "0.5 ", " 0.5",
                          "0..5", ".", "nan", "inf", "0x1"}) {
    EXPECT_EQ(cli::ParseThreshold(bad), std::nullopt) << "'" << bad << "'";
  }
}

TEST(NumbersTest, ParseDuration) {
  EXPECT_EQ(ParseDuration("30s"), std::chrono::seconds(30));
  EXPECT_EQ(ParseDuration("10m"), std::chrono::seconds(600));
  EXPECT_EQ(ParseDuration("1h"), std::chrono::seconds(3600));
  for (const char* bad : {"", "s", "0s", "10", "10x", "1.5m", "-1s", " 5s", "05s"}) {
    EXPECT_EQ(ParseDuration(bad), std::nullopt) << "'" << bad << "'";
  }
}

}  // namespace
}  // namespace dds::cli
