#include "dds/cli/json.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace dds::cli {
namespace {

TEST(JsonTest, ObjectsArraysAndCommas) {
  JsonWriter w;
  w.BeginObject();
  w.Key("a");
  w.Int(-1);
  w.Key("b");
  w.BeginArray();
  w.Uint(1);
  w.Uint(2);
  w.BeginObject();
  w.EndObject();
  w.EndArray();
  w.Key("c");
  w.Bool(true);
  w.Key("d");
  w.Null();
  w.EndObject();
  EXPECT_EQ(w.Take(), R"({"a":-1,"b":[1,2,{}],"c":true,"d":null})");
}

TEST(JsonTest, StringsAreEscaped) {
  JsonWriter w;
  w.BeginArray();
  w.String("quote\" backslash\\ newline\n tab\t");
  w.String(std::string("nul\0end", 7));
  w.String("caf\xc3\xa9");  // valid UTF-8 passes through
  w.EndArray();
  EXPECT_EQ(w.Take(),
            "[\"quote\\\" backslash\\\\ newline\\n tab\\t\",\"nul\\u0000end\",\"caf\xc3\xa9\"]");
}

TEST(JsonTest, InvalidUtf8BecomesReplacementCharacters) {
  // RFC 8259 section 8.1: JSON text is UTF-8. File names are arbitrary bytes (D41), so bytes that
  // are not part of a valid UTF-8 sequence are written as U+FFFD, one per byte.
  const std::string fffd = "\xef\xbf\xbd";
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x93\x81",
       "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x93\x81"},  // valid
      {"a\xff"
       "b",
       "a" + fffd + "b"},                               // never valid in UTF-8
      {"\xc0\x80", fffd + fffd},                        // overlong NUL
      {"\xed\xa0\x80", fffd + fffd + fffd},             // UTF-16 surrogate
      {"\xf4\x90\x80\x80", fffd + fffd + fffd + fffd},  // above U+10FFFF
      {"\xe2\x82", fffd + fffd},                        // truncated sequence
      {"\x80", fffd},                                   // lone continuation byte
  };
  for (const auto& [in, out] : cases) {
    JsonWriter w;
    w.String(in);
    EXPECT_EQ(w.Take(), "\"" + out + "\"") << ::testing::PrintToString(in);
    EXPECT_EQ(IsValidUtf8(in), in == out) << ::testing::PrintToString(in);
  }
}

TEST(JsonTest, DoublesAndNonFiniteValues) {
  JsonWriter w;
  w.BeginArray();
  w.Double(1.5);
  w.Double(0.0);
  w.Double(std::numeric_limits<double>::infinity());
  w.Double(std::nan(""));
  w.EndArray();
  EXPECT_EQ(w.Take(), "[1.5,0,null,null]");
}

TEST(JsonTest, LargeIntegers) {
  JsonWriter w;
  w.BeginArray();
  w.Uint(18446744073709551615ULL);
  w.Int(std::numeric_limits<int64_t>::min());
  w.EndArray();
  EXPECT_EQ(w.Take(), "[18446744073709551615,-9223372036854775808]");
}

}  // namespace
}  // namespace dds::cli
