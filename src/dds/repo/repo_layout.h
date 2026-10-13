#pragma once

#include <string>

#include "dds/fs/fs.h"

namespace dds {

// The paths inside a repository directory (ARCHITECTURE.md section 3; ON_DISK_FORMAT.md
// section 2).
struct RepoLayout {
  std::string root;

  std::string config() const { return JoinPath(root, "config"); }
  std::string lock() const { return JoinPath(root, "lock"); }
  std::string containers() const { return JoinPath(root, "containers"); }
  std::string snapshots() const { return JoinPath(root, "snapshots"); }
  std::string tmp() const { return JoinPath(root, "tmp"); }
  std::string index_checkpoint() const { return JoinPath(root, "index.ckpt"); }
};

}  // namespace dds
