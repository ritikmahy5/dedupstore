#include "support/fs_conformance_env.h"

#include <memory>
#include <string>
#include <vector>

#include "dds/fs/crash_sim_fs.h"
#include "dds/fs/mem_fs.h"
#include "dds/fs/posix_fs.h"
#include "support/temp_dir.h"

namespace dds::testing {

namespace {

class PosixEnv final : public FsEnv {
 public:
  Fs& fs() override { return fs_; }
  std::string base() const override { return dir_.path(); }

 private:
  TempDir dir_;
  PosixFs fs_;
};

class MemEnv final : public FsEnv {
 public:
  MemEnv() { (void)fs_.CreateDir("/base"); }
  Fs& fs() override { return fs_; }
  std::string base() const override { return "/base"; }

 private:
  MemFs fs_;
};

// No crash is injected: the conformance suite checks that recording durability does not change
// what the running process sees.
class CrashSimEnv final : public FsEnv {
 public:
  CrashSimEnv() { (void)fs_.CreateDir("/base"); }
  Fs& fs() override { return fs_; }
  std::string base() const override { return "/base"; }

 private:
  CrashSimFs fs_;
};

}  // namespace

std::vector<FsEnvFactory> AllFsEnvFactories() {
  return {
      {"PosixFs", [] { return std::unique_ptr<FsEnv>(std::make_unique<PosixEnv>()); }},
      {"MemFs", [] { return std::unique_ptr<FsEnv>(std::make_unique<MemEnv>()); }},
      {"CrashSimFs", [] { return std::unique_ptr<FsEnv>(std::make_unique<CrashSimEnv>()); }},
  };
}

}  // namespace dds::testing
