#include "dds/repo/ingest_session.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "dds/fs/mem_fs.h"
#include "dds/repo/repository.h"
#include "support/fake_process_identity.h"
#include "support/random_data.h"

namespace dds {
namespace {

class IngestSessionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RepoParams params;
    params.container_target = kMinContainerTarget;  // 1 MiB: easy to fill
    ASSERT_TRUE(Repository::Init(fs_, "/r", params).ok());
    auto repo = Repository::Open(fs_, "/r", me_);
    ASSERT_TRUE(repo.ok());
    repo_ = std::move(repo).value();
  }
  RepoInternals& in() { return repo_->Internals(); }

  MemFs fs_;
  testing::FakeProcessIdentity me_{"h", 1, 1};
  std::unique_ptr<Repository> repo_;
};

TEST_F(IngestSessionTest, AMissReservesSoTheBlobIsStoredOnce) {
  IngestSession s(in(), 2);
  const auto data = testing::RandomBytes(1, 1000);
  const Fingerprint fp = FingerprintOf(BlobType::kData, data);
  EXPECT_EQ(s.Lookup(fp, Reserve::kOn), LookupResult::kMiss);
  // Another worker of the same session now sees a hit: the first will Put it (ARCHITECTURE 4.1).
  EXPECT_EQ(s.Lookup(fp, Reserve::kOn), LookupResult::kHit);
  ASSERT_TRUE(s.Put(fp, BlobType::kData, data, 0).ok());
  EXPECT_EQ(s.Lookup(fp, Reserve::kOn), LookupResult::kHit);
  EXPECT_EQ(s.counters().blobs_written, 1u);
}

TEST_F(IngestSessionTest, AMissWithoutReservationReservesNothing) {
  // A replication receiver's HAVE (D55): a miss must not make the blob look present.
  IngestSession s(in(), 1);
  const Fingerprint fp = FingerprintOf(BlobType::kData, testing::RandomBytes(2, 10));
  EXPECT_EQ(s.Lookup(fp, Reserve::kOff), LookupResult::kMiss);
  EXPECT_EQ(s.Lookup(fp, Reserve::kOff), LookupResult::kMiss);
}

TEST_F(IngestSessionTest, PublishAfterSeal) {
  // D11: nothing is in the index until its container is sealed.
  IngestSession s(in(), 1);
  const auto data = testing::RandomBytes(3, 1000);
  const Fingerprint fp = FingerprintOf(BlobType::kData, data);
  ASSERT_EQ(s.Lookup(fp, Reserve::kOn), LookupResult::kMiss);
  ASSERT_TRUE(s.Put(fp, BlobType::kData, data, 0).ok());
  EXPECT_FALSE(in().index.Lookup(fp).has_value());
  ASSERT_TRUE(s.SealAll().ok());
  ASSERT_TRUE(in().index.Lookup(fp).has_value());
  EXPECT_EQ(in().containers.PublishedNotRetired().size(), 1u);
  // Still a hit after leaving the pending map, now through the index.
  EXPECT_EQ(s.Lookup(fp, Reserve::kOn), LookupResult::kHit);
}

TEST_F(IngestSessionTest, FullWritersSealAutomatically) {
  IngestSession s(in(), 1);
  for (uint64_t i = 0; i < 40; ++i) {  // 40 x 64 KiB > 1 MiB target
    const auto data = testing::RandomBytes(100 + i, 65536);
    const Fingerprint fp = FingerprintOf(BlobType::kData, data);
    ASSERT_EQ(s.Lookup(fp, Reserve::kOn), LookupResult::kMiss);
    ASSERT_TRUE(s.Put(fp, BlobType::kData, data, 0).ok());
  }
  EXPECT_GE(s.counters().containers_sealed, 2u);
  EXPECT_GE(in().index.Size(), 30u);  // sealed ones are already published
}

TEST_F(IngestSessionTest, CommitSealsEverythingThenWritesTheManifest) {
  IngestSession s(in(), 2);
  const auto data = testing::RandomBytes(4, 100);
  const Fingerprint fp = FingerprintOf(BlobType::kData, data);
  ASSERT_EQ(s.Lookup(fp, Reserve::kOn), LookupResult::kMiss);
  ASSERT_TRUE(s.Put(fp, BlobType::kData, data, 1).ok());
  Manifest m;
  m.root = fp;
  auto id = s.Commit(EncodeManifest(m));
  ASSERT_TRUE(id.ok()) << id.status().ToString();
  EXPECT_TRUE(in().index.Lookup(fp).has_value());
  auto listed = in().snapshots.List();
  ASSERT_TRUE(listed.ok());
  EXPECT_EQ(listed->ids, std::vector<SnapshotId>{*id});
}

TEST_F(IngestSessionTest, TwoSessionsStoringTheSameBlobCountADuplicate) {
  const auto data = testing::RandomBytes(5, 100);
  const Fingerprint fp = FingerprintOf(BlobType::kData, data);
  IngestSession a(in(), 1);
  IngestSession b(in(), 1);
  ASSERT_EQ(a.Lookup(fp, Reserve::kOn), LookupResult::kMiss);
  ASSERT_EQ(b.Lookup(fp, Reserve::kOn), LookupResult::kMiss);  // neither sealed yet (D11, D32)
  ASSERT_TRUE(a.Put(fp, BlobType::kData, data, 0).ok());
  ASSERT_TRUE(b.Put(fp, BlobType::kData, data, 0).ok());
  ASSERT_TRUE(a.SealAll().ok());
  ASSERT_TRUE(b.SealAll().ok());
  EXPECT_EQ(a.counters().duplicates_published + b.counters().duplicates_published, 1u);
}

// The lookup protocol's guarantee (ARCHITECTURE.md 4.1) under the concurrency backup creates:
// every worker looks up every fingerprint, the one that misses stores the blob, and writers seal
// and publish as they fill. Each fingerprint must miss exactly once, whether a lookup finds it
// pending (reserved or written) or already in the index, so it is stored exactly once (D101).
TEST_F(IngestSessionTest, ConcurrentWorkersReserveEachFingerprintExactlyOnce) {
  constexpr size_t kThreads = 8;
  // 4 KiB each, about 16 MiB, so 1 MiB writers seal many times. Prime, so each worker's order
  // below (multiply by an odd number, add an offset, mod kBlobs) is a permutation.
  constexpr size_t kBlobs = 4001;
  for (uint64_t round = 0; round < 3; ++round) {
    std::vector<std::vector<std::byte>> blobs;
    std::vector<Fingerprint> fps;
    for (size_t i = 0; i < kBlobs; ++i) {
      blobs.push_back(testing::RandomBytes(1000000 * (round + 1) + i, 4096));
      fps.push_back(FingerprintOf(BlobType::kData, blobs.back()));
    }
    std::vector<std::atomic<int>> misses(kBlobs);
    std::atomic<bool> failed{false};
    IngestSession s(in(), kThreads);
    std::vector<std::thread> workers;
    for (size_t t = 0; t < kThreads; ++t) {
      workers.emplace_back([&, t] {
        // Each worker visits the blobs in its own order, so workers collide on every fingerprint.
        std::vector<size_t> order(kBlobs);
        for (size_t i = 0; i < kBlobs; ++i) order[i] = (i * (2 * t + 1) + 977 * t) % kBlobs;
        for (size_t i : order) {
          if (s.Lookup(fps[i], Reserve::kOn) == LookupResult::kHit) continue;
          misses[i].fetch_add(1);
          if (!s.Put(fps[i], BlobType::kData, blobs[i], t).ok()) failed = true;
        }
      });
    }
    for (std::thread& w : workers) w.join();
    ASSERT_FALSE(failed);
    for (size_t i = 0; i < kBlobs; ++i) {
      ASSERT_EQ(misses[i].load(), 1) << "blob " << i << " in round " << round;
    }
    ASSERT_TRUE(s.SealAll().ok());
    const IngestCounters c = s.counters();
    EXPECT_EQ(c.blobs_written, kBlobs);
    EXPECT_EQ(c.duplicates_published, 0u);
    EXPECT_GE(c.containers_sealed, 8u);
    for (const Fingerprint& fp : fps) EXPECT_TRUE(in().index.Lookup(fp).has_value());
  }
}

}  // namespace
}  // namespace dds
