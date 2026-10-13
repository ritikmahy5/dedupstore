#include "dds/repo/snapshot_store.h"

#include <gtest/gtest.h>

#include <string>

#include "dds/common/encoding.h"
#include "dds/fs/mem_fs.h"

namespace dds {
namespace {

class SnapshotStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    for (const char* d : {"/r", "/r/tmp", "/r/snapshots"}) ASSERT_TRUE(fs_.CreateDir(d).ok());
  }
  std::vector<std::byte> ManifestBytes(int64_t created) {
    Manifest m;
    m.created_at_ns = created;
    m.host = "h";
    return EncodeManifest(m);
  }
  MemFs fs_;
  SnapshotStore store_{fs_, RepoLayout{"/r"}};
};

TEST_F(SnapshotStoreTest, CommitListReadRemove) {
  const auto bytes = ManifestBytes(1);
  auto id = store_.Commit(bytes);
  ASSERT_TRUE(id.ok()) << id.status().ToString();
  EXPECT_EQ(*id, SnapshotIdOf(bytes));
  EXPECT_TRUE(*fs_.Exists("/r/snapshots/" + SnapshotIdToHex(*id) + ".snap"));
  EXPECT_TRUE(fs_.ListDir("/r/tmp")->empty());
  auto listing = store_.List();
  ASSERT_TRUE(listing.ok());
  EXPECT_EQ(listing->ids, std::vector<SnapshotId>{*id});
  auto m = store_.Read(*id);
  ASSERT_TRUE(m.ok()) << m.status().ToString();
  EXPECT_EQ(m->host, "h");
  ASSERT_TRUE(store_.Remove(*id).ok());
  EXPECT_TRUE(store_.List()->ids.empty());
  EXPECT_EQ(store_.Read(*id).status().code(), StatusCode::kNotFound);
  EXPECT_EQ(store_.Remove(*id).code(), StatusCode::kNotFound);
}

TEST_F(SnapshotStoreTest, CommitIsIdempotent) {
  const auto bytes = ManifestBytes(2);
  auto a = store_.Commit(bytes);
  auto b = store_.Commit(bytes);  // a replicated snapshot committed again (PROTOCOL.md 4.2 step 5)
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(*a, *b);
  EXPECT_EQ(store_.List()->ids.size(), 1u);
}

TEST_F(SnapshotStoreTest, ManifestUnderTheWrongNameIsCorruption) {
  const auto bytes = ManifestBytes(3);
  auto f = fs_.CreateNew("/r/snapshots/00000000000000000000000000000000.snap");
  ASSERT_TRUE(f.ok());
  ASSERT_TRUE((*f)->Append(bytes).ok());
  ASSERT_TRUE((*f)->Close().ok());
  auto m = store_.Read(SnapshotId{});
  EXPECT_EQ(m.status().code(), StatusCode::kCorruption);
}

TEST_F(SnapshotStoreTest, OtherFilesAreIgnored) {
  auto f = fs_.CreateNew("/r/snapshots/README");
  ASSERT_TRUE(f.ok());
  ASSERT_TRUE((*f)->Close().ok());
  auto listing = store_.List();
  ASSERT_TRUE(listing.ok());
  EXPECT_TRUE(listing->ids.empty());
  EXPECT_EQ(listing->ignored, std::vector<std::string>{"README"});
}

TEST_F(SnapshotStoreTest, ResolveByUniquePrefix) {
  auto a = store_.Commit(ManifestBytes(10));
  auto b = store_.Commit(ManifestBytes(11));
  ASSERT_TRUE(a.ok() && b.ok());
  const std::string ha = SnapshotIdToHex(*a);
  const std::string hb = SnapshotIdToHex(*b);
  EXPECT_EQ(*store_.Resolve(ha), *a);
  // The shortest unique prefix of at least 6 characters resolves.
  size_t common = 0;
  while (ha[common] == hb[common]) ++common;
  const size_t len = std::max<size_t>(6, common + 1);
  EXPECT_EQ(*store_.Resolve(ha.substr(0, len)), *a);
  EXPECT_EQ(store_.Resolve(ha.substr(0, 5)).status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(store_.Resolve("zzzzzz").status().code(), StatusCode::kInvalidArgument);
  // A prefix whose first digit differs from both ids matches nothing.
  std::string none = ha.substr(0, 6);
  for (char c : std::string("0123456789abcdef")) {
    if (c != ha[0] && c != hb[0]) {
      none[0] = c;
      break;
    }
  }
  EXPECT_EQ(store_.Resolve(none).status().code(), StatusCode::kNotFound);
}

}  // namespace
}  // namespace dds
