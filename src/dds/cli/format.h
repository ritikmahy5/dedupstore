#pragma once

#include <cstdint>
#include <string>

#include "dds/snapshot/tree.h"

namespace dds::cli {

// Human-readable forms for stderr output. `--json` output uses raw numbers instead.

// "2026-10-08T19:26:33Z" (UTC, whole seconds; the nanoseconds stay in --json).
std::string FormatUnixSeconds(int64_t sec);
std::string FormatTime(int64_t unix_nanos);
// "999 B", "1.5 KiB", "12.0 MiB", "3.1 GiB" (powers of 1024, as the size flags use).
std::string FormatBytes(uint64_t bytes);
// "drwxr-xr-x", "-rw-r--r--", "lrwxrwxrwx", as ls -l prints them.
std::string FormatMode(EntryKind kind, uint32_t mode);
// "file", "directory", "symlink" (the --json "kind" values).
const char* EntryKindName(EntryKind kind);

}  // namespace dds::cli
