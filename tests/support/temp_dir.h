#pragma once

#include <string>

namespace dds::testing {

// A fresh directory under the system temporary directory, removed with its contents when the
// object is destroyed. Test-only: library code never touches the file system except through
// dds::Fs (D24).
class TempDir {
 public:
  TempDir();
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }
  // path() + "/" + name.
  std::string Sub(const std::string& name) const;

 private:
  std::string path_;
};

}  // namespace dds::testing
