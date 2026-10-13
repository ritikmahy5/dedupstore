// `lock` record codec (ON_DISK_FORMAT.md section 4; TESTING.md 3.3).

#include <gtest/gtest.h>

#include <string>

#include "dds/repo/repo_lock.h"

namespace dds {
namespace {

LockRecord Example() {
  return {.host = "laptop.local",
          .pid = 4242,
          .pid_start = 1791331200123456,
          .acquired_at_ns = 1791331200999999999,
          .command = "dds backup /r /home"};
}

TEST(LockRecordTest, GoldenBytes) {
  EXPECT_EQ(EncodeLockRecord(Example()),
            "host=laptop.local\n"
            "pid=4242\n"
            "pid_start=1791331200123456\n"
            "acquired_at_ns=1791331200999999999\n"
            "command=dds backup /r /home\n");
}

TEST(LockRecordTest, RoundTrip) {
  auto back = DecodeLockRecord(EncodeLockRecord(Example()));
  ASSERT_TRUE(back.ok()) << back.status().ToString();
  EXPECT_EQ(back->host, "laptop.local");
  EXPECT_EQ(back->pid, 4242u);
  EXPECT_EQ(back->pid_start, 1791331200123456u);
  EXPECT_EQ(back->acquired_at_ns, 1791331200999999999);
  EXPECT_EQ(back->command, "dds backup /r /home");
}

TEST(LockRecordTest, CommandIsSanitized) {
  // ON_DISK_FORMAT.md section 4: the command is truncated to 512 bytes; a newline would break the
  // line structure, so it becomes a space.
  EXPECT_EQ(SanitizeLockCommand("a\nb\rc"), "a b c");
  EXPECT_EQ(SanitizeLockCommand(std::string(600, 'x')).size(), 512u);
  EXPECT_EQ(SanitizeLockCommand(""), "");
}

TEST(LockRecordTest, EverySingleBitFlipFailsOrReencodesExactly) {
  // The lock has no checksum: a flipped byte must either be rejected or decode to a record that
  // re-encodes to exactly the flipped bytes. Never a crash.
  const std::string good = EncodeLockRecord(Example());
  for (size_t byte = 0; byte < good.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string flipped = good;
      flipped[byte] = static_cast<char>(flipped[byte] ^ (1 << bit));
      auto decoded = DecodeLockRecord(flipped);
      if (decoded.ok()) {
        EXPECT_EQ(EncodeLockRecord(*decoded), flipped) << "byte " << byte << " bit " << bit;
      } else {
        EXPECT_EQ(decoded.status().code(), StatusCode::kCorruption);
      }
    }
  }
}

TEST(LockRecordTest, RejectsMalformedRecords) {
  for (const char* bad :
       {"", "host=a\n", "pid=1\nhost=a\npid_start=1\nacquired_at_ns=1\ncommand=\n",
        "host=\npid=1\npid_start=1\nacquired_at_ns=1\ncommand=\n",
        "host=a\npid=01\npid_start=1\nacquired_at_ns=1\ncommand=\n",
        "host=a\npid=1\npid_start=1\nacquired_at_ns=1\ncommand=x",
        "host=a\npid=1\npid_start=1\nacquired_at_ns=1\ncommand=\nextra=1\n"}) {
    EXPECT_EQ(DecodeLockRecord(bad).status().code(), StatusCode::kCorruption) << bad;
  }
  EXPECT_TRUE(DecodeLockRecord("host=a\npid=1\npid_start=1\nacquired_at_ns=-5\ncommand=\n").ok());
}

}  // namespace
}  // namespace dds
