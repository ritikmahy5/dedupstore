#include "dds/common/encoding.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "support/test_seed.h"

namespace dds {
namespace {

std::vector<std::byte> Bytes(std::initializer_list<int> values) {
  std::vector<std::byte> out;
  for (int v : values) out.push_back(static_cast<std::byte>(v));
  return out;
}

TEST(EncodingTest, StoresLittleEndian) {
  std::array<std::byte, 8> buf{};
  StoreU16(buf.data(), 0x0201);
  EXPECT_EQ(std::vector<std::byte>(buf.begin(), buf.begin() + 2), Bytes({1, 2}));
  StoreU32(buf.data(), 0x04030201u);
  EXPECT_EQ(std::vector<std::byte>(buf.begin(), buf.begin() + 4), Bytes({1, 2, 3, 4}));
  StoreU64(buf.data(), 0x0807060504030201ULL);
  EXPECT_EQ(std::vector<std::byte>(buf.begin(), buf.end()), Bytes({1, 2, 3, 4, 5, 6, 7, 8}));
}

TEST(EncodingTest, LoadStoreRoundTripAtBoundaries) {
  std::array<std::byte, 8> buf{};
  for (uint16_t v :
       {uint16_t{0}, uint16_t{1}, uint16_t{0x7FFF}, uint16_t{0x8000}, uint16_t{0xFFFF}}) {
    StoreU16(buf.data(), v);
    EXPECT_EQ(LoadU16(buf.data()), v);
  }
  for (uint32_t v : {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu}) {
    StoreU32(buf.data(), v);
    EXPECT_EQ(LoadU32(buf.data()), v);
  }
  for (uint64_t v : {uint64_t{0}, uint64_t{1}, uint64_t{0x7FFFFFFFFFFFFFFF},
                     uint64_t{0x8000000000000000}, std::numeric_limits<uint64_t>::max()}) {
    StoreU64(buf.data(), v);
    EXPECT_EQ(LoadU64(buf.data()), v);
  }
}

TEST(EncodingTest, WriterReaderRoundTrip) {
  Writer w;
  w.U8(0xAB);
  w.U16(0xBEEF);
  w.U32(0xDEADBEEF);
  w.U64(0x0123456789ABCDEFULL);
  w.I64(-1);
  w.I64(std::numeric_limits<int64_t>::min());
  w.Str16("");
  w.Str16("hello");
  const std::vector<std::byte> blob = Bytes({9, 8, 7});
  w.Bytes(blob);
  EXPECT_EQ(w.size(), 1u + 2 + 4 + 8 + 8 + 8 + 2 + 7 + 3);

  Reader r(w.buffer());
  uint8_t u8 = 0;
  uint16_t u16 = 0;
  uint32_t u32 = 0;
  uint64_t u64 = 0;
  int64_t i64 = 0;
  std::string s;
  std::span<const std::byte> view;
  ASSERT_TRUE(r.U8(&u8));
  EXPECT_EQ(u8, 0xAB);
  ASSERT_TRUE(r.U16(&u16));
  EXPECT_EQ(u16, 0xBEEF);
  ASSERT_TRUE(r.U32(&u32));
  EXPECT_EQ(u32, 0xDEADBEEFu);
  ASSERT_TRUE(r.U64(&u64));
  EXPECT_EQ(u64, 0x0123456789ABCDEFULL);
  ASSERT_TRUE(r.I64(&i64));
  EXPECT_EQ(i64, -1);
  ASSERT_TRUE(r.I64(&i64));
  EXPECT_EQ(i64, std::numeric_limits<int64_t>::min());
  ASSERT_TRUE(r.Str16(&s));
  EXPECT_EQ(s, "");
  ASSERT_TRUE(r.Str16(&s));
  EXPECT_EQ(s, "hello");
  ASSERT_TRUE(r.Bytes(3, &view));
  EXPECT_EQ(std::vector<std::byte>(view.begin(), view.end()), blob);
  EXPECT_TRUE(r.empty());
  EXPECT_EQ(r.offset(), w.size());
}

TEST(EncodingTest, NegativeI64IsTwosComplement) {
  Writer w;
  w.I64(-2);
  EXPECT_EQ(w.buffer(), Bytes({0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
}

TEST(EncodingTest, Str16MaximumLength) {
  std::string big(0xFFFF, 'x');
  Writer w;
  w.Str16(big);
  Reader r(w.buffer());
  std::string out;
  ASSERT_TRUE(r.Str16(&out));
  EXPECT_EQ(out, big);
}

TEST(EncodingTest, ReaderRejectsShortInputWithoutAdvancing) {
  const std::vector<std::byte> three = Bytes({1, 2, 3});
  Reader r(three);
  uint32_t u32 = 7;
  EXPECT_FALSE(r.U32(&u32));
  EXPECT_EQ(u32, 7u);  // untouched
  EXPECT_EQ(r.offset(), 0u);
  uint16_t u16 = 0;
  EXPECT_TRUE(r.U16(&u16));
  EXPECT_FALSE(r.U16(&u16));
  EXPECT_EQ(r.offset(), 2u);
  EXPECT_EQ(r.remaining(), 1u);
  std::span<const std::byte> view;
  EXPECT_FALSE(r.Bytes(2, &view));
  EXPECT_TRUE(r.Bytes(1, &view));
  EXPECT_TRUE(r.empty());
  uint8_t u8 = 0;
  EXPECT_FALSE(r.U8(&u8));
}

TEST(EncodingTest, ReaderRejectsStr16LongerThanInput) {
  // Length prefix says 5 bytes, only 2 follow.
  const std::vector<std::byte> data = Bytes({5, 0, 'a', 'b'});
  Reader r(data);
  std::string s = "unchanged";
  EXPECT_FALSE(r.Str16(&s));
  EXPECT_EQ(s, "unchanged");
  EXPECT_EQ(r.offset(), 0u);
}

TEST(EncodingTest, ReaderHugeLengthDoesNotOverflow) {
  const std::vector<std::byte> data = Bytes({1, 2});
  Reader r(data);
  std::span<const std::byte> view;
  EXPECT_FALSE(r.Bytes(std::numeric_limits<size_t>::max(), &view));
  EXPECT_TRUE(r.Bytes(1, &view));
  EXPECT_FALSE(r.Bytes(std::numeric_limits<size_t>::max(), &view));
}

TEST(EncodingTest, RandomRoundTrip) {
  for (uint64_t seed : testing::TestSeeds(5)) {
    SCOPED_TRACE(testing::SeedMessage(seed));
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> values;
    Writer w;
    for (int i = 0; i < 1000; ++i) {
      uint64_t v = rng();
      values.push_back(v);
      w.U64(v);
      w.U32(static_cast<uint32_t>(v));
    }
    Reader r(w.buffer());
    for (uint64_t v : values) {
      uint64_t got64 = 0;
      uint32_t got32 = 0;
      ASSERT_TRUE(r.U64(&got64));
      ASSERT_TRUE(r.U32(&got32));
      EXPECT_EQ(got64, v);
      EXPECT_EQ(got32, static_cast<uint32_t>(v));
    }
    EXPECT_TRUE(r.empty());
  }
}

TEST(EncodingTest, AsBytesAndAsCharsAreViews) {
  std::string s = "abc";
  std::span<const std::byte> b = AsBytes(s);
  ASSERT_EQ(b.size(), 3u);
  EXPECT_EQ(b[0], std::byte{'a'});
  EXPECT_EQ(AsChars(b), "abc");
}

}  // namespace
}  // namespace dds
