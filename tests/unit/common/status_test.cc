#include "dds/common/status.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "dds/common/result.h"

namespace dds {
namespace {

TEST(StatusTest, DefaultIsOk) {
  Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kOk);
  EXPECT_EQ(s.message(), "");
  EXPECT_EQ(s.ToString(), "OK");
}

TEST(StatusTest, FactoriesSetCodeAndMessage) {
  EXPECT_EQ(Status::NotFound("a").code(), StatusCode::kNotFound);
  EXPECT_EQ(Status::Corruption("a").code(), StatusCode::kCorruption);
  EXPECT_EQ(Status::IoError("a").code(), StatusCode::kIoError);
  EXPECT_EQ(Status::InvalidArgument("a").code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(Status::AlreadyExists("a").code(), StatusCode::kAlreadyExists);
  EXPECT_EQ(Status::Busy("a").code(), StatusCode::kBusy);
  EXPECT_EQ(Status::Aborted("a").code(), StatusCode::kAborted);
  EXPECT_EQ(Status::Incompatible("a").code(), StatusCode::kIncompatible);
  Status s = Status::Corruption("containers/00000001.ctr: offset 32: bad CRC");
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.message(), "containers/00000001.ctr: offset 32: bad CRC");
  EXPECT_EQ(s.ToString(), "Corruption: containers/00000001.ctr: offset 32: bad CRC");
}

// The numeric values cross the wire in a replication ERROR message (PROTOCOL.md section 3).
TEST(StatusTest, CodeValuesAreTheWireContract) {
  EXPECT_EQ(static_cast<int>(StatusCode::kOk), 0);
  EXPECT_EQ(static_cast<int>(StatusCode::kNotFound), 1);
  EXPECT_EQ(static_cast<int>(StatusCode::kCorruption), 2);
  EXPECT_EQ(static_cast<int>(StatusCode::kIoError), 3);
  EXPECT_EQ(static_cast<int>(StatusCode::kInvalidArgument), 4);
  EXPECT_EQ(static_cast<int>(StatusCode::kAlreadyExists), 5);
  EXPECT_EQ(static_cast<int>(StatusCode::kBusy), 6);
  EXPECT_EQ(static_cast<int>(StatusCode::kAborted), 7);
  EXPECT_EQ(static_cast<int>(StatusCode::kIncompatible), 8);
}

TEST(StatusTest, CodeFromWireRoundTripsAndRejectsUnknown) {
  for (uint16_t v = 0; v <= 8; ++v) {
    auto code = StatusCodeFromWire(v);
    ASSERT_TRUE(code.has_value()) << v;
    EXPECT_EQ(static_cast<uint16_t>(*code), v);
  }
  EXPECT_FALSE(StatusCodeFromWire(9).has_value());
  EXPECT_FALSE(StatusCodeFromWire(0xFFFF).has_value());
}

TEST(StatusTest, FromCodeBuildsAnyCode) {
  Status s = Status::FromCode(StatusCode::kBusy, "held");
  EXPECT_EQ(s.code(), StatusCode::kBusy);
  EXPECT_EQ(s.message(), "held");
  EXPECT_TRUE(Status::FromCode(StatusCode::kOk, "").ok());
}

TEST(StatusTest, WithContextPrefixesErrorsOnly) {
  EXPECT_TRUE(Status().WithContext("ignored").ok());
  Status s = Status::Corruption("bad CRC").WithContext("containers/00000002.ctr");
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
  EXPECT_EQ(s.message(), "containers/00000002.ctr: bad CRC");
}

TEST(StatusTest, CodeNamesAreStable) {
  EXPECT_STREQ(StatusCodeName(StatusCode::kNotFound), "NotFound");
  EXPECT_STREQ(StatusCodeName(StatusCode::kIncompatible), "Incompatible");
}

Status FailsThenNeverReached(bool* reached) {
  DDS_TRY(Status::IoError("disk"));
  *reached = true;
  return Status();
}

Status PassesThrough(bool* reached) {
  DDS_TRY(Status());
  *reached = true;
  return Status();
}

TEST(StatusTest, TryReturnsEarlyOnError) {
  bool reached = false;
  Status s = FailsThenNeverReached(&reached);
  EXPECT_EQ(s.code(), StatusCode::kIoError);
  EXPECT_FALSE(reached);
  EXPECT_TRUE(PassesThrough(&reached).ok());
  EXPECT_TRUE(reached);
}

TEST(ResultTest, HoldsValue) {
  Result<int> r = 42;
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value(), 42);
  EXPECT_EQ(*r, 42);
  EXPECT_TRUE(r.status().ok());
}

TEST(ResultTest, HoldsError) {
  Result<int> r = Status::NotFound("no such snapshot");
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), StatusCode::kNotFound);
}

TEST(ResultTest, MoveOnlyValueAndConversion) {
  struct Base {
    virtual ~Base() = default;
    virtual int Get() const { return 1; }
  };
  struct Derived : Base {
    int Get() const override { return 2; }
  };
  auto make = []() -> Result<std::unique_ptr<Base>> { return std::make_unique<Derived>(); };
  Result<std::unique_ptr<Base>> r = make();
  ASSERT_TRUE(r.ok());
  EXPECT_EQ((*r)->Get(), 2);
  std::unique_ptr<Base> taken = std::move(r).value();
  EXPECT_EQ(taken->Get(), 2);
}

TEST(ResultTest, StringFromLiteral) {
  Result<std::string> r = "abc";
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, "abc");
  EXPECT_EQ(r->size(), 3u);
}

Result<int> ResultFails() {
  DDS_TRY(Status::Corruption("x"));
  return 1;
}

TEST(ResultTest, TryConvertsStatusToResult) {
  Result<int> r = ResultFails();
  EXPECT_EQ(r.status().code(), StatusCode::kCorruption);
}

TEST(ResultDeathTest, ValueOfErrorAborts) {
  Result<int> r = Status::IoError("x");
  EXPECT_DEATH((void)r.value(), "check failed");
}

TEST(ResultDeathTest, ErrorResultFromOkStatusAborts) {
  EXPECT_DEATH(
      {
        Result<int> r = Status();
        (void)r;
      },
      "check failed");
}

}  // namespace
}  // namespace dds
