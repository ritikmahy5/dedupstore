#pragma once

#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "dds/fs/fs.h"

namespace dds::testing {

// One Fs implementation plus an empty base directory to work in, for the conformance suite.
class FsEnv {
 public:
  virtual ~FsEnv() = default;
  virtual Fs& fs() = 0;
  virtual std::string base() const = 0;
};

struct FsEnvFactory {
  std::string name;
  std::function<std::unique_ptr<FsEnv>()> make;
};

// GoogleTest prints each test parameter, and gtest_discover_tests puts that text into the ctest
// name. Without this printer the name is the struct's raw bytes, pointers included, so it is
// unreadable and changes from build to build.
inline void PrintTo(const FsEnvFactory& factory, std::ostream* os) { *os << factory.name; }

// PosixFs in a temporary directory, MemFs, and CrashSimFs with no crash injected.
std::vector<FsEnvFactory> AllFsEnvFactories();

}  // namespace dds::testing
