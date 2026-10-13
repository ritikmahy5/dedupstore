#include "dds/cli/args.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace dds::cli {
namespace {

const std::vector<FlagSpec> kFlags = {
    {"threads", true}, {"tag", true}, {"json", false}, {"dry-run", false}};

std::optional<ParsedArgs> Parse(std::vector<std::string> words, std::string* error) {
  return ParseArgs(words, kFlags, error);
}

TEST(ArgsTest, PositionalsAndFlagsInAnyOrder) {
  std::string error;
  auto p = Parse({"repo", "--threads", "4", "path", "--json", "--tag=nightly run"}, &error);
  ASSERT_TRUE(p.has_value()) << error;
  EXPECT_EQ(p->positional, (std::vector<std::string>{"repo", "path"}));
  EXPECT_EQ(p->Get("threads"), "4");
  EXPECT_EQ(p->Get("tag"), "nightly run");
  EXPECT_TRUE(p->Has("json"));
  EXPECT_EQ(p->Get("json"), "");
  EXPECT_FALSE(p->Has("dry-run"));
  EXPECT_EQ(p->Get("dry-run"), std::nullopt);
}

TEST(ArgsTest, EmptyValueWithEquals) {
  std::string error;
  auto p = Parse({"--tag="}, &error);
  ASSERT_TRUE(p.has_value()) << error;
  EXPECT_EQ(p->Get("tag"), "");
}

TEST(ArgsTest, UnknownFlagIsUsageError) {
  std::string error;
  EXPECT_FALSE(Parse({"--bogus"}, &error).has_value());
  EXPECT_NE(error.find("--bogus"), std::string::npos) << error;
}

TEST(ArgsTest, RepeatedFlagIsUsageError) {
  std::string error;
  EXPECT_FALSE(Parse({"--json", "--json"}, &error).has_value());
  EXPECT_NE(error.find("--json"), std::string::npos) << error;
}

TEST(ArgsTest, MissingValueIsUsageError) {
  std::string error;
  EXPECT_FALSE(Parse({"repo", "--threads"}, &error).has_value());
  EXPECT_NE(error.find("--threads"), std::string::npos) << error;
}

TEST(ArgsTest, ValueForSwitchIsUsageError) {
  std::string error;
  EXPECT_FALSE(Parse({"--json=yes"}, &error).has_value());
  EXPECT_NE(error.find("--json"), std::string::npos) << error;
}

TEST(ArgsTest, DoubleDashEndsFlags) {
  std::string error;
  auto p = Parse({"--json", "--", "--threads", "-x"}, &error);
  ASSERT_TRUE(p.has_value()) << error;
  EXPECT_TRUE(p->Has("json"));
  EXPECT_EQ(p->positional, (std::vector<std::string>{"--threads", "-x"}));
}

TEST(ArgsTest, SingleDashWordIsPositional) {
  std::string error;
  auto p = Parse({"-", "-v"}, &error);
  ASSERT_TRUE(p.has_value()) << error;
  EXPECT_EQ(p->positional, (std::vector<std::string>{"-", "-v"}));
}

TEST(ArgsTest, ValueMayStartWithDashes) {
  std::string error;
  auto p = Parse({"--tag", "--weird"}, &error);
  ASSERT_TRUE(p.has_value()) << error;
  EXPECT_EQ(p->Get("tag"), "--weird");
}

}  // namespace
}  // namespace dds::cli
