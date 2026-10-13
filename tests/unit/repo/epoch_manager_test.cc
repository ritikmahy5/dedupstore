// EpochManager (ARCHITECTURE.md 6.1): sessions, readers, and BeginGc's E_gc and E_min.

#include "dds/repo/epoch_manager.h"

#include <gtest/gtest.h>

namespace dds {
namespace {

TEST(EpochManagerTest, BeginGcAdvancesAndTakesTheOldestActiveSession) {
  EpochManager e;
  EXPECT_EQ(e.Current(), 1u);
  const Epoch s1 = e.BeginSession();  // starts at 1
  EXPECT_EQ(e.Advance(), 2u);
  const Epoch s2 = e.BeginSession();  // starts at 2
  auto gc = e.BeginGc();
  EXPECT_EQ(gc.e_gc, 3u);
  EXPECT_EQ(gc.e_min, s1);  // the oldest active session
  e.EndSession(s1);
  gc = e.BeginGc();
  EXPECT_EQ(gc.e_gc, 4u);
  EXPECT_EQ(gc.e_min, s2);
  e.EndSession(s2);
  gc = e.BeginGc();
  EXPECT_EQ(gc.e_min, gc.e_gc);  // no session: E_min = E_gc
  EXPECT_EQ(e.active_sessions(), 0u);
}

TEST(EpochManagerTest, OldestReaderTracksActiveReaders) {
  EpochManager e;
  EXPECT_EQ(e.OldestReader(), std::nullopt);
  {
    ReadGuard first(e);  // enters at 1
    e.Advance();
    {
      ReadGuard second(e);  // enters at 2
      EXPECT_EQ(e.OldestReader(), 1u);
    }
    EXPECT_EQ(e.OldestReader(), 1u);
  }
  EXPECT_EQ(e.OldestReader(), std::nullopt);
}

}  // namespace
}  // namespace dds
