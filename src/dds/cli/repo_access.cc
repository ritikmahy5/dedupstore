#include "dds/cli/repo_access.h"

#include "dds/common/log.h"
#include "dds/common/process_identity.h"
#include "dds/common/strings.h"
#include "dds/fs/posix_fs.h"

namespace dds::cli {

Result<std::unique_ptr<Repository>> OpenRepository(const std::string& path,
                                                   std::string_view command) {
  // One of each per process: the repository keeps references to them until it is closed.
  static PosixFs fs;
  static const PosixProcessIdentity self;
  OpenOptions options;
  options.command = std::string(command);
  return Repository::Open(fs, path, self, options);
}

void CloseRepository(Repository& repo) {
  const Status closed = repo.Close();
  if (!closed.ok()) LogWarn(StrCat("could not release the repository lock: ", closed.ToString()));
}

}  // namespace dds::cli
