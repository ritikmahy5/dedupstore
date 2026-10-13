#pragma once

#include <functional>
#include <string>
#include <string_view>

// Test hooks (D35, D75; ARCHITECTURE.md section 10). Compiled only with DDS_ENABLE_TEST_HOOKS
// (the debug, asan, tsan, and fuzz presets); in release both macros compile to nothing.
//
// DDS_MUTATION("name"): true while a test has switched mutation `name` on. The real code uses it to
// break one durability or concurrency rule on purpose, so a test can check that its harness
// notices (TESTING.md 3.6-3.7):
//
//   if (!DDS_MUTATION("skip_container_dir_sync")) DDS_TRY(fs.SyncDir(dir));
//
// DDS_SYNC_POINT("name"): runs the callback a test registered for `name`, for example to pause this
// thread on a latch and force an interleaving.
//
// Every name must be in the list in test_hooks.cc; an unknown name is a bug (a typo would make a
// mutation test pass without breaking anything) and aborts.
#if defined(DDS_ENABLE_TEST_HOOKS)
#define DDS_MUTATION(name) (::dds::test_hooks::MutationOn(name))
#define DDS_SYNC_POINT(name) (::dds::test_hooks::SyncPoint(name))
#else
#define DDS_MUTATION(name) (false)
#define DDS_SYNC_POINT(name) static_cast<void>(0)
#endif

namespace dds::test_hooks {

#if defined(DDS_ENABLE_TEST_HOOKS)
inline constexpr bool kEnabled = true;
#else
inline constexpr bool kEnabled = false;  // tests that need hooks skip themselves
#endif

bool MutationOn(std::string_view name);
void SyncPoint(std::string_view name);

// Switches mutation `name` on for this object's lifetime.
class ScopedMutation {
 public:
  explicit ScopedMutation(std::string name);
  ~ScopedMutation();
  ScopedMutation(const ScopedMutation&) = delete;
  ScopedMutation& operator=(const ScopedMutation&) = delete;

 private:
  std::string name_;
};

// Runs `callback` (on the thread that reaches it) whenever sync point `name` is reached, for this
// object's lifetime. The callback runs with no hook lock held, so it may block.
class ScopedSyncPoint {
 public:
  ScopedSyncPoint(std::string name, std::function<void()> callback);
  ~ScopedSyncPoint();
  ScopedSyncPoint(const ScopedSyncPoint&) = delete;
  ScopedSyncPoint& operator=(const ScopedSyncPoint&) = delete;

 private:
  std::string name_;
};

}  // namespace dds::test_hooks
