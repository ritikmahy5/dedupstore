// Repository init (ARCHITECTURE.md 7.7) and open (7.6), and round trips through put, seal, and
// reopen.

#include "dds/repo/repository.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "dds/common/encoding.h"
#include "dds/fs/mem_fs.h"
#include "dds/store/container_format.h"
#include "support/fake_process_identity.h"
#include "support/random_data.h"

namespace dds {
namespace {

using Blobs = std::vector<std::pair<BlobType, std::vector<std::byte>>>;

class RepositoryTest : public ::testing::Test {
 protected:
  void WriteFile(const std::string& path, std::string_view text) {
    auto f = fs_.CreateNew(path);
    ASSERT_TRUE(f.ok()) << f.status().ToString();
    ASSERT_TRUE((*f)->Append(AsBytes(text)).ok());
    ASSERT_TRUE((*f)->Close().ok());
  }
  std::unique_ptr<Repository> OpenOk() {
    auto repo = Repository::Open(fs_, "/r", me_);
    EXPECT_TRUE(repo.ok()) << repo.status().ToString();
    return repo.ok() ? std::move(repo).value() : nullptr;
  }
  // Writes `blobs` into one container through the seal protocol and publishes them.
  SealedContainer Put(Repository& repo, const Blobs& blobs) {
    RepoInternals& in = repo.Internals();
    ContainerWriter w = in.NewWriter();
    for (const auto& [type, payload] : blobs) w.Add(FingerprintOf(type, payload), type, payload);
    auto sealed = w.Seal();
    EXPECT_TRUE(sealed.ok()) << sealed.status().ToString();
    in.PublishSealed(*sealed, in.epochs.Current());
    return *sealed;
  }
  std::map<Fingerprint, Location> IndexContents(Repository& repo) {
    std::map<Fingerprint, Location> out;
    for (size_t s = 0; s < ChunkIndex::kShardCount; ++s) {
      repo.Internals().index.ForEachInShard(s,
                                            [&](const IndexEntry& e) { out[e.fp] = e.location(); });
    }
    return out;
  }

  MemFs fs_;
  testing::FakeProcessIdentity me_{"host-a", 100, 5000};
};

TEST_F(RepositoryTest, InitCreatesAnEmptyRepositoryThatOpens) {
  ASSERT_TRUE(Repository::Init(fs_, "/r", RepoParams{}).ok());
  for (const char* p : {"/r/config", "/r/tmp", "/r/containers", "/r/snapshots"}) {
    EXPECT_TRUE(*fs_.Exists(p)) << p;
  }
  auto repo = OpenOk();
  ASSERT_NE(repo, nullptr);
  EXPECT_EQ(repo->config().chunker.avg_size, 8192u);
  EXPECT_EQ(repo->config().container_target, 4u << 20);
  EXPECT_EQ(repo->open_report().containers, 0u);
  EXPECT_EQ(repo->Internals().index.Size(), 0u);
  EXPECT_EQ(repo->Internals().epochs.Current(), 1u);
}

TEST_F(RepositoryTest, TwoInitsGetDifferentRepoIds) {
  ASSERT_TRUE(fs_.CreateDir("/a").ok());
  ASSERT_TRUE(Repository::Init(fs_, "/a/one", RepoParams{}).ok());
  ASSERT_TRUE(Repository::Init(fs_, "/a/two", RepoParams{}).ok());
  auto one = Repository::Open(fs_, "/a/one", me_);
  auto two = Repository::Open(fs_, "/a/two", me_);
  ASSERT_TRUE(one.ok() && two.ok());
  EXPECT_NE((*one)->config().repo_id, (*two)->config().repo_id);
}

TEST_F(RepositoryTest, InitRefusesAnythingButAnEmptyOrCrashedInitDirectory) {
  ASSERT_TRUE(Repository::Init(fs_, "/r", RepoParams{}).ok());
  EXPECT_EQ(Repository::Init(fs_, "/r", RepoParams{}).status().code(), StatusCode::kAlreadyExists);
  ASSERT_TRUE(fs_.CreateDir("/full").ok());
  WriteFile("/full/notes.txt", "mine");
  EXPECT_EQ(Repository::Init(fs_, "/full", RepoParams{}).status().code(),
            StatusCode::kAlreadyExists);
  WriteFile("/afile", "x");
  EXPECT_EQ(Repository::Init(fs_, "/afile", RepoParams{}).status().code(),
            StatusCode::kAlreadyExists);
  ASSERT_TRUE(fs_.CreateDir("/busy").ok());
  ASSERT_TRUE(fs_.CreateDir("/busy/containers").ok());
  WriteFile("/busy/containers/00000001.ctr", "x");
  EXPECT_EQ(Repository::Init(fs_, "/busy", RepoParams{}).status().code(),
            StatusCode::kAlreadyExists);
}

TEST_F(RepositoryTest, InitAcceptsWhatACrashedInitLeft) {
  // A crash during init can leave the directories and a partial tmp/config.tmp, but never config
  // (its rename is the commit point, D52). Init must then simply run again.
  ASSERT_TRUE(fs_.CreateDir("/r").ok());
  ASSERT_TRUE(fs_.CreateDir("/r/tmp").ok());
  ASSERT_TRUE(fs_.CreateDir("/r/containers").ok());
  WriteFile("/r/tmp/config.tmp", "dedupstore-con");
  EXPECT_EQ(Repository::Open(fs_, "/r", me_).status().code(), StatusCode::kNotFound);
  ASSERT_TRUE(Repository::Init(fs_, "/r", RepoParams{}).ok());
  EXPECT_NE(OpenOk(), nullptr);
}

TEST_F(RepositoryTest, InitRejectsInvalidParameters) {
  RepoParams params;
  params.chunker.avg_size = 1000;
  EXPECT_EQ(Repository::Init(fs_, "/r", params).status().code(), StatusCode::kInvalidArgument);
  params = RepoParams{};
  params.container_target = 1000;
  EXPECT_EQ(Repository::Init(fs_, "/r", params).status().code(), StatusCode::kInvalidArgument);
  EXPECT_FALSE(*fs_.Exists("/r"));  // nothing created
}

TEST_F(RepositoryTest, OpenOfANonRepositoryIsNotFound) {
  EXPECT_EQ(Repository::Open(fs_, "/nope", me_).status().code(), StatusCode::kNotFound);
  ASSERT_TRUE(fs_.CreateDir("/empty").ok());
  EXPECT_EQ(Repository::Open(fs_, "/empty", me_).status().code(), StatusCode::kNotFound);
}

TEST_F(RepositoryTest, OpenEmptiesTmp) {
  ASSERT_TRUE(Repository::Init(fs_, "/r", RepoParams{}).ok());
  WriteFile("/r/tmp/00000007.ctr.tmp", "half a container");
  WriteFile("/r/tmp/0123456789abcdef0123456789abcdef.snap.tmp", "half a manifest");
  auto repo = OpenOk();
  EXPECT_TRUE(fs_.ListDir("/r/tmp")->empty());
}

TEST_F(RepositoryTest, BlobsOfEverySizeSurviveReopen) {
  // put/seal/reopen/get for blobs of size 1, max_size, and a 64 MiB metadata blob.
  ASSERT_TRUE(Repository::Init(fs_, "/r", RepoParams{}).ok());
  const Blobs blobs = {{BlobType::kData, testing::RandomBytes(1, 1)},
                       {BlobType::kData, testing::RandomBytes(2, 65536)},
                       {BlobType::kTree, testing::RandomBytes(3, kMaxMetadataBlobBytes)}};
  {
    auto repo = OpenOk();
    Put(*repo, blobs);
    ASSERT_TRUE(repo->Close().ok());
  }
  auto repo = OpenOk();
  EXPECT_EQ(repo->open_report().blobs, 3u);
  for (const auto& [type, payload] : blobs) {
    auto loc = repo->Internals().index.Lookup(FingerprintOf(type, payload));
    ASSERT_TRUE(loc.has_value());
    EXPECT_EQ(loc->type, type);
    auto bytes = repo->Internals().containers.Read(*loc);
    ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();
    EXPECT_EQ(*bytes, payload);
  }
}

TEST_F(RepositoryTest, ReopenRebuildsTheIndexAndTheHighestCopyWins) {
  // Same fingerprints, types, lengths and CRCs; same locations except for blobs stored
  // twice, whose rebuilt location is the higher container id; duplicates are counted.
  ASSERT_TRUE(Repository::Init(fs_, "/r", RepoParams{}).ok());
  std::map<Fingerprint, Location> before;
  const auto shared = testing::RandomBytes(99, 3000);
  const Fingerprint shared_fp = FingerprintOf(BlobType::kData, shared);
  uint32_t second_id = 0;
  {
    auto repo = OpenOk();
    Blobs a, b;
    for (uint64_t i = 0; i < 40; ++i)
      a.push_back({BlobType::kData, testing::RandomBytes(i, 500 + i)});
    for (uint64_t i = 40; i < 70; ++i)
      b.push_back({BlobType::kRecipe, testing::RandomBytes(i, 200)});
    a.push_back({BlobType::kData, shared});
    b.push_back({BlobType::kData, shared});  // a second session stored the same new blob (D32)
    Put(*repo, a);
    second_id = Put(*repo, b).id;
    before = IndexContents(*repo);
    EXPECT_LT(before.at(shared_fp).container_id, second_id);  // Publish kept the first copy
    ASSERT_TRUE(repo->Close().ok());
  }
  auto repo = OpenOk();
  const auto after = IndexContents(*repo);
  ASSERT_EQ(after.size(), before.size());
  for (const auto& [fp, loc] : before) {
    ASSERT_EQ(after.count(fp), 1u);
    const Location& rebuilt = after.at(fp);
    EXPECT_EQ(rebuilt.type, loc.type);
    EXPECT_EQ(rebuilt.length, loc.length);
    EXPECT_EQ(rebuilt.crc32c, loc.crc32c);
    if (fp == shared_fp) {
      EXPECT_EQ(rebuilt.container_id, second_id);
    } else {
      EXPECT_EQ(rebuilt, loc);
    }
  }
  EXPECT_EQ(repo->open_report().duplicate_copies, 1u);
  EXPECT_EQ(repo->open_report().containers, 2u);
}

TEST_F(RepositoryTest, CorruptContainerIsReportedSkippedAndNeverReused) {
  ASSERT_TRUE(Repository::Init(fs_, "/r", RepoParams{}).ok());
  uint32_t bad_id = 0;
  {
    auto repo = OpenOk();
    Put(*repo, {{BlobType::kData, testing::RandomBytes(1, 100)}});
    bad_id = Put(*repo, {{BlobType::kData, testing::RandomBytes(2, 100)}}).id;
    ASSERT_TRUE(repo->Close().ok());
  }
  const std::string bad_path = "/r/containers/" + ContainerFileName(bad_id);
  auto data = ReadWholeFile(fs_, bad_path);
  ASSERT_TRUE(data.ok());
  (*data)[data->size() - 40] ^= std::byte{1};  // inside the footer
  ASSERT_TRUE(fs_.Remove(bad_path).ok());
  WriteFile(bad_path, AsChars(*data));
  auto repo = OpenOk();
  ASSERT_EQ(repo->open_report().bad_containers.size(), 1u);
  EXPECT_NE(repo->open_report().bad_containers[0].find(ContainerFileName(bad_id)),
            std::string::npos);
  EXPECT_EQ(repo->open_report().containers, 1u);
  EXPECT_EQ(repo->Internals().index.Size(), 1u);
  EXPECT_GT(repo->Internals().containers.NextId(), bad_id);  // its name is never reused
}

TEST_F(RepositoryTest, UnknownFilesAreIgnoredAndReported) {
  ASSERT_TRUE(Repository::Init(fs_, "/r", RepoParams{}).ok());
  WriteFile("/r/containers/notes.txt", "hi");
  auto repo = OpenOk();
  EXPECT_EQ(repo->open_report().ignored_files, (std::vector<std::string>{"notes.txt"}));
}

TEST_F(RepositoryTest, OneOpenAtATime) {
  ASSERT_TRUE(Repository::Init(fs_, "/r", RepoParams{}).ok());
  auto first = OpenOk();
  testing::FakeProcessIdentity other("host-a", 200, 6000);
  other.SetAlive(100, 5000, true);
  EXPECT_EQ(Repository::Open(fs_, "/r", other).status().code(), StatusCode::kBusy);
  ASSERT_TRUE(first->Close().ok());
  EXPECT_TRUE(Repository::Open(fs_, "/r", other).ok());
}

TEST_F(RepositoryTest, UnknownConfigVersionIsIncompatible) {
  ASSERT_TRUE(fs_.CreateDir("/r").ok());
  ASSERT_TRUE(fs_.CreateDir("/r/tmp").ok());
  WriteFile("/r/config", "dedupstore-config=3\ncrc32c=00000000\n");
  EXPECT_EQ(Repository::Open(fs_, "/r", me_).status().code(), StatusCode::kIncompatible);
  EXPECT_FALSE(*fs_.Exists("/r/lock"));  // a failed open releases the lock
}

}  // namespace
}  // namespace dds
