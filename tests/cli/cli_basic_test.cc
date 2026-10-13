// CLI tests (TESTING.md 3.9): run the built `dds` binary and check exit codes and output.

#include <gtest/gtest.h>

#include <string>

#include "support/run_dds.h"

namespace dds {
namespace {

using testing::RunDds;

TEST(CliBasicTest, VersionPrintsNameAndVersion) {
  auto run = RunDds({"--version"});
  EXPECT_EQ(run.exit_code, 0) << run.err;
  EXPECT_EQ(run.out.rfind("dds ", 0), 0u) << run.out;
  EXPECT_NE(run.out.find("0.1.0"), std::string::npos) << run.out;
}

TEST(CliBasicTest, HelpListsUsage) {
  auto run = RunDds({"--help"});
  EXPECT_EQ(run.exit_code, 0) << run.err;
  EXPECT_NE(run.out.find("usage: dds"), std::string::npos) << run.out;
  EXPECT_NE(run.out.find("--version"), std::string::npos) << run.out;
}

TEST(CliBasicTest, NoArgumentsIsUsageError) {
  auto run = RunDds({});
  EXPECT_EQ(run.exit_code, 1);
  EXPECT_NE(run.err.find("usage: dds"), std::string::npos) << run.err;
}

TEST(CliBasicTest, UnknownCommandIsUsageError) {
  auto run = RunDds({"frobnicate"});
  EXPECT_EQ(run.exit_code, 1);
  EXPECT_NE(run.err.find("frobnicate"), std::string::npos) << run.err;
}

TEST(CliBasicTest, UnknownGlobalFlagIsUsageError) {
  auto run = RunDds({"--bogus"});
  EXPECT_EQ(run.exit_code, 1);
  EXPECT_NE(run.err.find("--bogus"), std::string::npos) << run.err;
}

}  // namespace
}  // namespace dds
