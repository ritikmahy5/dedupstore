// Crash consistency (TESTING.md 3.6; G2, G3): every scenario survives a crash after
// any of its file-system calls, under every post-crash disk A1-A3 allow (sampled with 20 seeds per
// crash point, including both extremes), and each named mutation of the durability protocol is
// caught.

#include <gtest/gtest.h>

#include <cstdio>
#include <initializer_list>
#include <string>
#include <thread>

#include "crash/crash_harness.h"
#include "dds/common/clock.h"
#include "dds/common/test_hooks.h"
#include "dds/gc/gc_runner.h"
#include "dds/repl/receiver.h"
#include "dds/repl/sender.h"
#include "dds/repl/simulated_link.h"
#include "dds/repo/repository.h"
#include "support/fake_process_identity.h"
#include "support/random_data.h"

namespace dds {
namespace {

using testing::CrashReport;
using testing::ScenarioLog;

// Small enough that thousands of crash states check in seconds; still two containers per backup
// (files and trees), shared content, an empty file, a symlink, and nesting.
MemTree TreeA() {
  MemSourceTree t;
  t.AddDir("docs");
  t.AddFile("docs/notes", testing::RandomBytes(1, 3000));
  t.AddFile("docs/empty", {});
  t.AddDir("docs/deep");
  t.AddFile("docs/deep/big", testing::RandomBytes(2, 40000));
  t.AddFile("copy-of-big", testing::RandomBytes(2, 40000));
  t.AddSymlink("link", "docs/notes");
  return t.nodes();
}

MemTree TreeB() {  // TreeA with one file changed and one added: dedup hits and new blobs
  MemTree t = TreeA();
  t["docs/notes"].content = testing::RandomBytes(3, 3500);
  MemNode added = t["docs/empty"];
  added.content = testing::RandomBytes(4, 9000);
  t["added"] = added;
  return t;
}

// The operations scenarios are built from. Each updates the log only once its step returns OK.
class Steps {
 public:
  Steps(CrashSimFs& fs, ScenarioLog& log, RepoParams params = {})
      : fs_(fs), log_(log), params_(params) {}

  Status Init() {
    auto config = Repository::Init(fs_, testing::kRepoPath, params_, clock_);
    DDS_TRY(config.status());
    log_.init_returned = true;
    return Status::Ok();
  }
  // `checkpoint_stale` 1 makes Close write an index checkpoint whenever a container changed (D104).
  Status Open(size_t checkpoint_stale = 256) {
    OpenOptions options;
    options.clock = &clock_;
    options.checkpoint_stale_containers = checkpoint_stale;
    auto repo = Repository::Open(fs_, testing::kRepoPath, self_, options);
    DDS_TRY(repo.status());
    repo_ = std::move(repo).value();
    return Status::Ok();
  }
  Result<SnapshotId> Backup(const MemTree& tree) {
    clock_.Advance(1000);
    log_.backup_in_flight = tree;
    MemSourceTree source(tree);
    auto result = repo_->Backup(source, BackupOptions{});
    DDS_TRY(result.status());
    log_.committed[result->id] = tree;
    log_.backup_in_flight.reset();
    return result->id;
  }
  Status Remove(const SnapshotId& id) {
    log_.rm_in_flight = id;
    DDS_TRY(repo_->RemoveSnapshot(id));
    log_.committed.erase(id);
    log_.removed.insert(id);
    log_.rm_in_flight.reset();
    return Status::Ok();
  }
  // GC changes no snapshot, so the log needs nothing: every check of the scenario so far must
  // hold however far GC got.
  Status Gc(double threshold) {
    GcRunner gc(*repo_);
    GcOptions options;
    options.threshold = threshold;
    auto report = gc.Run(options);
    DDS_TRY(report.status());
    last_gc_ = *report;
    return Status::Ok();
  }
  const GcReport& last_gc() const { return last_gc_; }
  // Receives snapshot `id` of `source` into this (crash-simulated) repository over a
  // SimulatedLink, the receiver on its own thread, as `dds serve` would run it.
  Status Receive(Repository& source, const SnapshotId& id, const MemTree& tree) {
    log_.backup_in_flight = tree;
    auto [to_receiver, to_sender] = MakeSimulatedLink({});
    ReceiverOptions options;
    options.idle_timeout = std::chrono::milliseconds(10000);
    std::thread serve(
        [&, end = to_sender.get()] { (void)ServeReplication(*repo_, *end, options); });
    SenderOptions sender;
    sender.reply_timeout = std::chrono::milliseconds(10000);
    auto sent = Replicate(source, id, *to_receiver, sender);
    to_receiver->Close();
    serve.join();
    DDS_TRY(sent.status());
    log_.committed[id] = tree;
    log_.backup_in_flight.reset();
    return Status::Ok();
  }
  Status Close() { return repo_->Close(); }
  const OpenReport& open_report() const { return repo_->open_report(); }

 private:
  CrashSimFs& fs_;
  ScenarioLog& log_;
  const RepoParams params_;
  ManualClock clock_{1700000000000000000, 1};
  testing::FakeProcessIdentity self_{"h", 1, 1};
  std::unique_ptr<Repository> repo_;
  GcReport last_gc_;
};

Status InitScenario(CrashSimFs& fs, ScenarioLog& log) { return Steps(fs, log).Init(); }

Status OneBackupScenario(CrashSimFs& fs, ScenarioLog& log) {
  Steps s(fs, log);
  DDS_TRY(s.Init());
  DDS_TRY(s.Open());
  DDS_TRY(s.Backup(TreeA()).status());
  return s.Close();
}

Status BackupAfterBackupScenario(CrashSimFs& fs, ScenarioLog& log) {
  Steps s(fs, log);
  DDS_TRY(s.Init());
  DDS_TRY(s.Open());
  DDS_TRY(s.Backup(TreeA()).status());
  DDS_TRY(s.Backup(TreeB()).status());
  return s.Close();
}

Status RmScenario(CrashSimFs& fs, ScenarioLog& log) {
  Steps s(fs, log);
  DDS_TRY(s.Init());
  DDS_TRY(s.Open());
  auto first = s.Backup(TreeA());
  DDS_TRY(first.status());
  DDS_TRY(s.Backup(TreeB()).status());
  DDS_TRY(s.Remove(*first));
  return s.Close();
}

// Explores `scenario` and expects no violations, and that each outcome in `both_seen` was reached.
MemTree Unrelated() {
  MemSourceTree t;
  t.AddFile("other", testing::RandomBytes(30, 20000));
  return t.nodes();
}

// TESTING.md 3.6's GC scenario: deletions and compactions. TreeA's containers are partly live
// once it is removed (TreeB shares most of it), so GC at 1.0 compacts them; the unrelated
// snapshot's containers are wholly dead, so GC deletes them.
Status GcSteps(Steps& s) {
  DDS_TRY(s.Init());
  DDS_TRY(s.Open());
  auto first = s.Backup(TreeA());
  DDS_TRY(first.status());
  DDS_TRY(s.Backup(TreeB()).status());
  auto unrelated = s.Backup(Unrelated());
  DDS_TRY(unrelated.status());
  DDS_TRY(s.Remove(*first));
  DDS_TRY(s.Remove(*unrelated));
  DDS_TRY(s.Gc(1.0));
  return s.Close();
}

Status GcScenario(CrashSimFs& fs, ScenarioLog& log) {
  Steps s(fs, log);
  return GcSteps(s);
}

// TESTING.md 3.6's replication receiver commit: a destination that already holds one snapshot
// receives a second, similar one (some blobs present, some sent).
Status ReplicationScenario(CrashSimFs& fs, ScenarioLog& log) {
  MemFs source_fs;
  DDS_TRY(Repository::Init(source_fs, "/src", RepoParams{}).status());
  testing::FakeProcessIdentity source_self("src", 7, 7);
  auto source = Repository::Open(source_fs, "/src", source_self);
  DDS_TRY(source.status());
  MemSourceTree a(TreeA());
  auto first = (*source)->Backup(a, BackupOptions{});
  DDS_TRY(first.status());
  MemSourceTree b(TreeB());
  auto second = (*source)->Backup(b, BackupOptions{});
  DDS_TRY(second.status());

  Steps s(fs, log);
  DDS_TRY(s.Init());
  DDS_TRY(s.Open());
  DDS_TRY(s.Receive(**source, first->id, TreeA()));
  DDS_TRY(s.Receive(**source, second->id, TreeB()));
  return s.Close();
}

// D104: Close writes an index checkpoint, the next open loads it, GC deletes and compacts
// containers it covers, and Close writes a second one. Every post-crash open loads whichever
// checkpoint survived, or none, so each must agree with the containers on disk.
Status CheckpointSteps(Steps& s) {
  DDS_TRY(s.Init());
  DDS_TRY(s.Open(/*checkpoint_stale=*/1));
  auto first = s.Backup(TreeA());
  DDS_TRY(first.status());
  DDS_TRY(s.Backup(TreeB()).status());
  auto unrelated = s.Backup(Unrelated());
  DDS_TRY(unrelated.status());
  DDS_TRY(s.Close());
  DDS_TRY(s.Open(1));
  if (s.open_report().checkpoint_containers == 0) return Status::Aborted("checkpoint not loaded");
  DDS_TRY(s.Remove(*first));
  DDS_TRY(s.Remove(*unrelated));
  DDS_TRY(s.Gc(1.0));
  return s.Close();
}

Status CheckpointScenario(CrashSimFs& fs, ScenarioLog& log) {
  Steps s(fs, log);
  return CheckpointSteps(s);
}

// D107: the same protocol over compressed containers (format version 2), with blobs that zstd
// shrinks and blobs it cannot, through backups, a removal, and a GC that compacts and deletes.
MemTree Compressible(uint64_t seed, size_t size) {
  MemSourceTree t;
  std::vector<std::byte> text = testing::RandomBytes(seed, size);
  for (std::byte& b : text) b = static_cast<std::byte>('a' + (static_cast<int>(b) & 3));
  t.AddFile("text", text);
  t.AddFile("noise", testing::RandomBytes(seed + 1, size / 2));
  return t.nodes();
}

Status CompressedScenario(CrashSimFs& fs, ScenarioLog& log) {
  RepoParams params;
  params.compression = {Codec::kZstd, 3};
  Steps s(fs, log, params);
  DDS_TRY(s.Init());
  DDS_TRY(s.Open());
  auto first = s.Backup(Compressible(40, 30000));
  DDS_TRY(first.status());
  MemTree second = Compressible(40, 30000);
  second["added"] = Compressible(41, 9000)["text"];
  DDS_TRY(s.Backup(second).status());
  DDS_TRY(s.Remove(*first));
  DDS_TRY(s.Gc(1.0));
  return s.Close();
}

TEST(CrashTest, CheckpointScenarioLoadsACheckpointAndGcsCoveredContainers) {
  CrashSimFs fs;
  ScenarioLog log;
  Steps s(fs, log);
  ASSERT_TRUE(CheckpointSteps(s).ok());
  EXPECT_GE(s.last_gc().compact.containers_deleted, 1u);
  EXPECT_GE(s.last_gc().compact.containers_compacted, 1u);
}

TEST(CrashTest, GcScenarioDeletesAndCompacts) {
  CrashSimFs fs;
  ScenarioLog log;
  Steps s(fs, log);
  ASSERT_TRUE(GcSteps(s).ok());
  EXPECT_GE(s.last_gc().compact.containers_deleted, 1u);
  EXPECT_GE(s.last_gc().compact.containers_compacted, 1u);
}

void ExpectNoViolations(const char* name, const testing::Scenario& scenario,
                        std::initializer_list<const char*> both_seen) {
  const CrashReport r = testing::ExploreCrashes(scenario);
  // TESTING.md 3.6: each scenario reports the number of crash states it explored.
  std::printf("[crash] %s: %llu crash points, %llu states checked, %llu distinct\n", name,
              static_cast<unsigned long long>(r.crash_points),
              static_cast<unsigned long long>(r.states),
              static_cast<unsigned long long>(r.distinct_states));
  ::testing::Test::RecordProperty("crash_points", static_cast<int>(r.crash_points));
  ::testing::Test::RecordProperty("crash_states", static_cast<int>(r.states));
  for (const auto& [outcome, count] : r.outcomes) {
    std::printf("[crash]   %s: %llu\n", outcome.c_str(), static_cast<unsigned long long>(count));
  }
  EXPECT_GT(r.crash_points, 1u);
  for (const char* outcome : both_seen) {
    EXPECT_GT(r.outcomes.count(outcome), 0u) << "never reached: " << outcome;
  }
  EXPECT_TRUE(r.violations.empty())
      << r.violations.size()
      << " violations, first: " << (r.violations.empty() ? "" : r.violations.front());
}

constexpr const char* kPresent = "in-flight backup present";
constexpr const char* kAbsent = "in-flight backup absent";

TEST(CrashTest, Init) { ExpectNoViolations("init", InitScenario, {"no repository"}); }
TEST(CrashTest, OneBackup) {
  ExpectNoViolations("one backup", OneBackupScenario, {"no repository", kPresent, kAbsent});
}
TEST(CrashTest, BackupAfterBackup) {
  ExpectNoViolations("backup after backup", BackupAfterBackupScenario, {kPresent, kAbsent});
}
TEST(CrashTest, GcWithDeletionsAndCompactions) {
  ExpectNoViolations("gc", GcScenario, {kPresent, kAbsent, "removal done", "removal undone"});
}

TEST(CrashTest, IndexCheckpoint) {
  ExpectNoViolations("index checkpoint", CheckpointScenario,
                     {kPresent, kAbsent, "removal done", "removal undone"});
}

TEST(CrashTest, CompressedBackupsAndGc) {
  ExpectNoViolations("compressed", CompressedScenario,
                     {kPresent, kAbsent, "removal done", "removal undone"});
}

TEST(CrashTest, Rm) {
  ExpectNoViolations("rm", RmScenario, {kPresent, kAbsent, "removal done", "removal undone"});
}

TEST(CrashTest, ReplicationReceiverCommit) {
  ExpectNoViolations("replication", ReplicationScenario, {kPresent, kAbsent});
}

// Teeth (TESTING.md 3.6): each mutation breaks one ordering of the protocol, and the harness must
// report it. If one went unnoticed, the harness would be too weak to trust its passes.
void ExpectCaught(const char* mutation, const testing::Scenario& scenario) {
  if (!test_hooks::kEnabled) GTEST_SKIP() << "built without DDS_ENABLE_TEST_HOOKS";
  test_hooks::ScopedMutation on(mutation);
  // Detection needs violating states, not all of them: 6 seeds per crash point (the two extremes
  // and 4 random) keep these tests fast; the scenarios themselves use 20.
  const CrashReport r = testing::ExploreCrashes(scenario, /*seeds=*/6);
  std::printf("[crash] mutation %s: %zu violations in %llu states\n", mutation, r.violations.size(),
              static_cast<unsigned long long>(r.states));
  ASSERT_FALSE(r.violations.empty()) << mutation << " went undetected";
  std::printf("[crash]   first: %s\n", r.violations.front().c_str());
}

// Several 1 MiB containers per backup, so crash states include a backup whose containers are only
// partly durable.
Status MultiContainerScenario(CrashSimFs& fs, ScenarioLog& log) {
  RepoParams params;
  params.container_target = kMinContainerTarget;
  Steps s(fs, log, params);
  MemSourceTree big;
  for (uint64_t i = 0; i < 3; ++i) {
    big.AddFile("f" + std::to_string(i), testing::RandomBytes(10 + i, 900000));
  }
  MemTree second = big.nodes();
  second["f1"].content = testing::RandomBytes(20, 700000);
  DDS_TRY(s.Init());
  DDS_TRY(s.Open());
  auto first = s.Backup(big.nodes());
  DDS_TRY(first.status());
  DDS_TRY(s.Backup(second).status());
  DDS_TRY(s.Remove(*first));
  return s.Close();
}

TEST(CrashSlowTest, MultiContainerBackups) {
  ExpectNoViolations("multi-container", MultiContainerScenario,
                     {kPresent, kAbsent, "removal done", "removal undone"});
}

TEST(CrashMutationTest, ManifestBeforeSealIsCaught) {
  ExpectCaught("manifest_before_seal", OneBackupScenario);
}
TEST(CrashMutationTest, SkipContainerDirSyncIsCaught) {
  ExpectCaught("skip_container_dir_sync", OneBackupScenario);
}
TEST(CrashMutationTest, GcUnlinkBeforeCopySyncIsCaught) {
  ExpectCaught("gc_unlink_before_copy_sync", GcScenario);
}

}  // namespace
}  // namespace dds
