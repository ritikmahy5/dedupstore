#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "dds/common/result.h"
#include "dds/repo/repository.h"

namespace dds::cli {

// Opens the repository at `path` for one command, as this process, through the real file system.
// `command` (for example "dds backup") is recorded in the lock file (D16).
Result<std::unique_ptr<Repository>> OpenRepository(const std::string& path,
                                                   std::string_view command);

// Releases the repository lock and logs a warning if that fails. The command's own result
// stands: a lock left behind by an exited process is taken over by the next open (D16).
void CloseRepository(Repository& repo);

}  // namespace dds::cli
