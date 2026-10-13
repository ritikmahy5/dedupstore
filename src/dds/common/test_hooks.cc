#include "dds/common/test_hooks.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <span>

#include "dds/common/check.h"
#include "dds/common/ordered_mutex.h"
#include "dds/common/strings.h"

namespace dds::test_hooks {

namespace {

// Every hook name the code uses. Mutations: TESTING.md 3.6-3.7. Sync points: ARCHITECTURE.md
// section 10. Names starting with "test." exist only for the hooks' own unit tests.
constexpr std::string_view kMutations[] = {
    "manifest_before_seal",         // IngestSession::Commit writes the manifest before sealing
    "skip_container_dir_sync",      // ContainerWriter::Seal skips SyncDir(containers/)
    "gc_unlink_before_copy_sync",   // GC unlinks a compacted container before its copies are sealed
    "no_epoch_protection",          // GC ignores last_ref when deciding liveness
    "candidate_before_publish",     // GC's candidates include sealed, unpublished containers
    "retire_without_grace",         // ReclaimRetired unlinks without waiting for readers
    "stamp_assign",                 // index stamps assign instead of taking the max
    "have_miss_reserves",           // a replication HAVE miss reserves the fingerprint
    "checkpoint_next_id_ignored",   // open ignores the index checkpoint's next container id
    "reuse_without_stamp",          // a backup reuses a parent's pieces without stamping them
    "subtree_check_without_stamp",  // HAVE_TREES finds a subtree whole without stamping it
};
constexpr std::string_view kSyncPoints[] = {
    "seal.before_publish",     // between ContainerWriter::Seal and index publication
    "commit.before_manifest",  // after sealing, before the manifest is written
    "gc.after_plan",           // between GC's plan and its per-entry decisions
    "gc.after_relocate",       // after copy-forward relocation, before retirement
    "read.after_lookup",       // between a guarded index lookup and its container read
    "backup.after_reuse",      // after a reused file's blobs were looked up, before it counts
};

void CheckKnown(std::string_view name, std::span<const std::string_view> known, const char* what) {
  if (name.starts_with("test.")) return;
  if (std::find(known.begin(), known.end(), name) == known.end()) {
    internal::Fatal(__FILE__, __LINE__, StrCat("unknown ", what, " '", name, "'"));
  }
}

struct Registry {
  // How many ScopedMutations exist. MutationOn is called on per-chunk paths (index stamping), so it
  // answers from this counter without locking when no test has switched anything on. Acquire and
  // release pair the increment in ScopedMutation with the load, so a thread that sees the count
  // also sees the inserted name.
  std::atomic<int> active_mutations{0};
  OrderedMutex mu{lock_level::kLeaf, "test_hooks"};
  std::multiset<std::string, std::less<>> mutations DDS_GUARDED_BY(mu);
  std::map<std::string, std::function<void()>, std::less<>> sync_points DDS_GUARDED_BY(mu);
};

Registry& GetRegistry() {
  static Registry registry;
  return registry;
}

}  // namespace

bool MutationOn(std::string_view name) {
  CheckKnown(name, kMutations, "mutation");
  Registry& r = GetRegistry();
  if (r.active_mutations.load(std::memory_order_acquire) == 0) return false;
  MutexLock lock(r.mu);
  return r.mutations.find(name) != r.mutations.end();
}

void SyncPoint(std::string_view name) {
  CheckKnown(name, kSyncPoints, "sync point");
  Registry& r = GetRegistry();
  std::function<void()> callback;
  {
    MutexLock lock(r.mu);
    auto it = r.sync_points.find(name);
    if (it == r.sync_points.end()) return;
    callback = it->second;
  }
  callback();  // outside the lock: it may block on a latch
}

ScopedMutation::ScopedMutation(std::string name) : name_(std::move(name)) {
  CheckKnown(name_, kMutations, "mutation");
  Registry& r = GetRegistry();
  MutexLock lock(r.mu);
  r.mutations.insert(name_);
  r.active_mutations.fetch_add(1, std::memory_order_release);
}

ScopedMutation::~ScopedMutation() {
  Registry& r = GetRegistry();
  MutexLock lock(r.mu);
  r.mutations.erase(r.mutations.find(name_));
  r.active_mutations.fetch_sub(1, std::memory_order_release);
}

ScopedSyncPoint::ScopedSyncPoint(std::string name, std::function<void()> callback)
    : name_(std::move(name)) {
  CheckKnown(name_, kSyncPoints, "sync point");
  Registry& r = GetRegistry();
  MutexLock lock(r.mu);
  DDS_CHECK(r.sync_points.emplace(name_, std::move(callback)).second);  // one callback per point
}

ScopedSyncPoint::~ScopedSyncPoint() {
  Registry& r = GetRegistry();
  MutexLock lock(r.mu);
  r.sync_points.erase(name_);
}

}  // namespace dds::test_hooks
