#include "crash/crash_harness.h"

#include <set>

#include "dds/common/clock.h"
#include "dds/common/encoding.h"
#include "dds/common/strings.h"
#include "dds/hash/blake3_hasher.h"
#include "dds/repo/repository.h"
#include "dds/snapshot/restore_sink.h"
#include "support/fake_process_identity.h"
#include "support/tree_compare.h"

namespace dds::testing {

namespace {

// A digest of everything on a disk, to count distinct crash states.
void DigestDir(MemFs& fs, const std::string& dir, Blake3Hasher& h) {
  auto names = fs.ListDir(dir);
  if (!names.ok()) return;
  for (const std::string& name : *names) {
    const std::string path = JoinPath(dir == "/" ? "" : dir, name);
    h.Update(AsBytes(path));
    if (fs.ListDir(path).ok()) {
      h.Update(AsBytes("/\n"));
      DigestDir(fs, path, h);
    } else {
      const auto data = ReadWholeFile(fs, path);
      h.Update(AsBytes(StrCat(":", data.ok() ? data->size() : 0, "\n")));
      if (data.ok()) h.Update(*data);
    }
  }
}

std::string DiskDigest(MemFs& fs) {
  Blake3Hasher h;
  DigestDir(fs, "/", h);
  const auto digest = h.Finalize();
  return std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
}

// Checks one post-crash disk against what the scenario had finished. Appends violations.
class StateChecker {
 public:
  StateChecker(MemFs& disk, const ScenarioLog& log, CrashReport& report)
      : disk_(disk), log_(log), report_(report) {}

  void Check() {
    // The crashed process (pid 1) is dead; this one (pid 2) takes its lock over (D16).
    FakeProcessIdentity self("h", 2, 2);
    OpenOptions options;
    options.clock = &clock_;
    auto repo = Repository::Open(disk_, kRepoPath, self, options);
    if (!repo.ok()) {
      if (log_.init_returned || repo.status().code() != StatusCode::kNotFound) {
        return Fail(StrCat("open failed: ", repo.status().ToString()));
      }
      // Init never returned and left no config: init must work again (ARCHITECTURE.md 7.7).
      Saw("no repository");
      auto again = Repository::Init(disk_, kRepoPath, RepoParams{}, clock_);
      if (!again.ok())
        Fail(StrCat("init after a crashed init failed: ", again.status().ToString()));
      return;
    }
    CheckOpened(**repo);
  }

 private:
  void Fail(std::string what) { failures.push_back(std::move(what)); }
  void Saw(const std::string& outcome) { ++report_.outcomes[outcome]; }

 public:
  std::vector<std::string> failures;

 private:
  void CheckOpened(Repository& repo) {
    auto tmp = disk_.ListDir(JoinPath(kRepoPath, "tmp"));
    if (!tmp.ok() || !tmp->empty()) Fail("tmp/ is not empty after open");
    VerifyOptions deep;
    deep.deep = true;
    auto report = repo.Verify(deep);
    if (!report.ok()) return Fail(StrCat("verify could not run: ", report.status().ToString()));
    for (const std::string& e : report->errors) Fail(StrCat("verify: ", e));
    auto listed = repo.ListSnapshots();
    if (!listed.ok()) return Fail(StrCat("listing failed: ", listed.status().ToString()));
    std::set<SnapshotId> present;
    for (const SnapshotInfo& s : *listed) present.insert(s.id);
    for (const auto& [id, tree] : log_.committed) {
      const bool removing = log_.rm_in_flight == id;
      if (removing) Saw(present.count(id) == 0 ? "removal done" : "removal undone");
      if (present.count(id) == 0) {
        if (!removing) Fail(StrCat("committed snapshot ", SnapshotIdToHex(id), " is missing"));
        continue;
      }
      CheckRestores(repo, id, tree);
    }
    for (const SnapshotId& id : log_.removed) {
      if (present.count(id) != 0)
        Fail(StrCat("removed snapshot ", SnapshotIdToHex(id), " is back"));
    }
    size_t unexpected = 0;
    for (const SnapshotId& id : present) {
      if (log_.committed.count(id) != 0 || log_.removed.count(id) != 0) continue;
      if (!log_.backup_in_flight.has_value() || ++unexpected > 1) {
        Fail(StrCat("unexpected snapshot ", SnapshotIdToHex(id)));
        continue;
      }
      CheckRestores(repo, id, *log_.backup_in_flight);  // the in-flight backup, complete
    }
    if (log_.backup_in_flight.has_value()) {
      Saw(unexpected == 1 ? "in-flight backup present" : "in-flight backup absent");
    }
  }

  void CheckRestores(Repository& repo, const SnapshotId& id, const MemTree& expected) {
    MemRestoreSink sink;
    auto restored = repo.Restore(id, sink, RestoreOptions{});
    if (!restored.ok()) {
      return Fail(StrCat("restore ", SnapshotIdToHex(id), ": ", restored.status().ToString()));
    }
    for (const std::string& d : DiffMemTrees(expected, sink.nodes())) {
      Fail(StrCat("restore ", SnapshotIdToHex(id), ": ", d));
    }
  }

  MemFs& disk_;
  const ScenarioLog& log_;
  CrashReport& report_;
  ManualClock clock_{1800000000000000000, 1};
};

}  // namespace

CrashReport ExploreCrashes(const Scenario& scenario, uint64_t seeds) {
  CrashReport report;
  uint64_t total_calls = 0;
  {
    CrashSimFs fs;
    ScenarioLog log;
    const Status dry = scenario(fs, log);
    if (!dry.ok()) {
      report.violations.push_back(StrCat("the scenario fails without a crash: ", dry.ToString()));
      return report;
    }
    total_calls = fs.mutating_calls();
  }
  std::set<std::string> digests;
  for (uint64_t n = 0; n <= total_calls; ++n) {
    CrashSimFs fs;
    fs.CrashAfter(n);
    ScenarioLog log;
    (void)scenario(fs, log);  // stops with IoError at the crash point (or completes, at n = K)
    ++report.crash_points;
    for (uint64_t seed = 0; seed < seeds; ++seed) {
      auto disk = fs.Materialize(seed);
      digests.insert(DiskDigest(*disk));
      StateChecker checker(*disk, log, report);
      checker.Check();
      for (std::string& v : checker.failures) {
        report.violations.push_back(StrCat("crash point ", n, ", seed ", seed, ": ", v));
      }
      ++report.states;
    }
  }
  report.distinct_states = digests.size();
  return report;
}

}  // namespace dds::testing
