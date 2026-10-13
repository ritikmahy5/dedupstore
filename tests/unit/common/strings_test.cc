#include "dds/common/strings.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dds {
namespace {

TEST(StringsTest, StrCatConcatenatesMixedTypes) {
  EXPECT_EQ(StrCat("a", 1, '-', std::string("b"), 2u, -3), "a1-b2-3");
  EXPECT_EQ(StrCat(), "");
  uint64_t big = 18446744073709551615ULL;
  EXPECT_EQ(StrCat(big), "18446744073709551615");
  uint8_t small = 200;  // printed as a number, not as a character
  EXPECT_EQ(StrCat(small), "200");
}

TEST(StringsTest, HexEncodeIsLowercase) {
  std::vector<std::byte> b = {std::byte{0x00}, std::byte{0xAB}, std::byte{0x7F}};
  EXPECT_EQ(HexEncode(b), "00ab7f");
  EXPECT_EQ(HexEncode({}), "");
}

TEST(StringsTest, HexDecodeRoundTrip) {
  std::array<std::byte, 3> out{};
  ASSERT_TRUE(HexDecode("00ab7f", out));
  EXPECT_EQ(out[0], std::byte{0x00});
  EXPECT_EQ(out[1], std::byte{0xAB});
  EXPECT_EQ(out[2], std::byte{0x7F});
}

TEST(StringsTest, HexDecodeRejectsBadInput) {
  std::array<std::byte, 2> out{};
  EXPECT_FALSE(HexDecode("abc", out));     // wrong length
  EXPECT_FALSE(HexDecode("abcdef", out));  // wrong length
  EXPECT_FALSE(HexDecode("zz00", out));    // not hex
  EXPECT_FALSE(HexDecode("AB00", out));    // uppercase is not canonical
  EXPECT_FALSE(HexDecode("0x00", out));
}

TEST(StringsTest, HexU32AndU64AreFixedWidth) {
  EXPECT_EQ(HexU32(0x2a), "0000002a");
  EXPECT_EQ(HexU32(0xffffffffu), "ffffffff");
  EXPECT_EQ(HexU64(0x6465647570737472ULL), "6465647570737472");
  EXPECT_EQ(HexU64(1), "0000000000000001");
}

TEST(StringsTest, ParseHexU32AndU64) {
  uint32_t v32 = 0;
  EXPECT_TRUE(ParseHexU32("0000002a", &v32));
  EXPECT_EQ(v32, 0x2au);
  EXPECT_FALSE(ParseHexU32("2a", &v32));
  EXPECT_FALSE(ParseHexU32("0000002A", &v32));
  uint64_t v64 = 0;
  EXPECT_TRUE(ParseHexU64("6465647570737472", &v64));
  EXPECT_EQ(v64, 0x6465647570737472ULL);
  EXPECT_FALSE(ParseHexU64("646564757073747", &v64));
}

TEST(StringsTest, ParseCanonicalDecimal) {
  uint64_t v = 99;
  EXPECT_TRUE(ParseCanonicalU64("0", &v));
  EXPECT_EQ(v, 0u);
  EXPECT_TRUE(ParseCanonicalU64("18446744073709551615", &v));
  EXPECT_EQ(v, 18446744073709551615ULL);
  EXPECT_FALSE(ParseCanonicalU64("18446744073709551616", &v));  // overflow
  EXPECT_FALSE(ParseCanonicalU64("007", &v));                   // leading zero
  EXPECT_FALSE(ParseCanonicalU64("+7", &v));
  EXPECT_FALSE(ParseCanonicalU64("", &v));
  EXPECT_FALSE(ParseCanonicalU64(" 7", &v));
  int64_t s = 0;
  EXPECT_TRUE(ParseCanonicalI64("-9223372036854775808", &s));
  EXPECT_EQ(s, INT64_MIN);
  EXPECT_TRUE(ParseCanonicalI64("9223372036854775807", &s));
  EXPECT_EQ(s, INT64_MAX);
  EXPECT_TRUE(ParseCanonicalI64("-1", &s));
  EXPECT_EQ(s, -1);
  EXPECT_FALSE(ParseCanonicalI64("-0", &s));  // zero has one spelling
  EXPECT_FALSE(ParseCanonicalI64("9223372036854775808", &s));
  EXPECT_FALSE(ParseCanonicalI64("-9223372036854775809", &s));
  EXPECT_FALSE(ParseCanonicalI64("-", &s));
}

}  // namespace
}  // namespace dds
