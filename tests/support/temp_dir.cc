#include "support/temp_dir.h"

#include <stdlib.h>  // mkdtemp (Linux)
#include <unistd.h>  // mkdtemp (macOS)

#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace dds::testing {

TempDir::TempDir() {
  std::string pattern = (std::filesystem::temp_directory_path() / "dds-test-XXXXXX").string();
  std::vector<char> buf(pattern.begin(), pattern.end());
  buf.push_back('\0');
  if (mkdtemp(buf.data()) == nullptr) {
    throw std::runtime_error("mkdtemp failed for " + pattern);
  }
  // Resolve symlinks (macOS /var -> /private/var) so paths compare equal to what the OS reports.
  path_ = std::filesystem::canonical(buf.data()).string();
}

TempDir::~TempDir() {
  std::error_code ec;
  // Restored trees may contain read-only directories; make everything writable first.
  for (auto it = std::filesystem::recursive_directory_iterator(
           path_, std::filesystem::directory_options::none, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_directory(ec) && !it->is_symlink(ec)) {
      std::filesystem::permissions(it->path(), std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::add, ec);
    }
  }
  std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::add, ec);
  std::filesystem::remove_all(path_, ec);
}

std::string TempDir::Sub(const std::string& name) const { return path_ + "/" + name; }

}  // namespace dds::testing
