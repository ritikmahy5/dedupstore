// Test hooks (D35, D75): mutations switch on only inside their scope, sync points run their
// callback, and unknown names abort.

#include "dds/common/test_hooks.h"

#include <gtest/gtest.h>

namespace dds {
namespace {

TEST(TestHooksTest, MutationIsOnOnlyInsideItsScope) {
  if (!test_hooks::kEnabled) {
    EXPECT_FALSE(DDS_MUTATION("manifest_before_seal"));  // release: always off
    GTEST_SKIP() << "built without DDS_ENABLE_TEST_HOOKS";
  }
  EXPECT_FALSE(DDS_MUTATION("manifest_before_seal"));
  {
    test_hooks::ScopedMutation on("manifest_before_seal");
    EXPECT_TRUE(DDS_MUTATION("manifest_before_seal"));
    EXPECT_FALSE(DDS_MUTATION("skip_container_dir_sync"));
    {
      test_hooks::ScopedMutation again("manifest_before_seal");
      EXPECT_TRUE(DDS_MUTATION("manifest_before_seal"));
    }
    EXPECT_TRUE(DDS_MUTATION("manifest_before_seal"));  // the outer scope still holds it
  }
  EXPECT_FALSE(DDS_MUTATION("manifest_before_seal"));
}

TEST(TestHooksTest, SyncPointRunsItsCallbackWhileRegistered) {
  if (!test_hooks::kEnabled) GTEST_SKIP() << "built without DDS_ENABLE_TEST_HOOKS";
  int calls = 0;
  DDS_SYNC_POINT("test.point");  // nothing registered: no effect
  {
    test_hooks::ScopedSyncPoint point("test.point", [&] { ++calls; });
    DDS_SYNC_POINT("test.point");
    DDS_SYNC_POINT("test.point");
  }
  DDS_SYNC_POINT("test.point");
  EXPECT_EQ(calls, 2);
}

TEST(TestHooksDeathTest, UnknownNamesAbort) {
  if (!test_hooks::kEnabled) GTEST_SKIP() << "built without DDS_ENABLE_TEST_HOOKS";
  EXPECT_DEATH((void)DDS_MUTATION("manifest_before_sael"), "unknown mutation");
  EXPECT_DEATH(DDS_SYNC_POINT("seal.befor_publish"), "unknown sync point");
}

}  // namespace
}  // namespace dds
