#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dds/common/result.h"
#include "dds/fs/fs.h"
#include "dds/store/container_format.h"
#include "dds/store/types.h"

namespace dds {

// Reads a container's trailer, header, and footer from `path` and validates them (ON_DISK_FORMAT.md
// 5.5). Used on open (one call per container, in parallel) and by GC to walk a container's
// entries.
Result<ContainerMetadata> ReadContainerMetadata(Fs& fs, const std::string& path,
                                                uint32_t expected_id, const RepoId& repo_id,
                                                uint32_t max_data_size);

// Reads the blob at `loc` from an open container file, checks the stored bytes' CRC32C (D26), and
// returns its content, decompressing a zstd frame (D107) whose content must be within the blob's
// limit (`max_data_size` for data, 64 MiB for trees and recipes). A CRC mismatch or a bad frame is
// Corruption naming `path` and the offset.
Result<std::vector<std::byte>> ReadBlob(const ReadableFile& file, const std::string& path,
                                        const Location& loc, uint32_t max_data_size);

// The blob's content length: `loc.length` for a blob stored as content; for a zstd frame, the size
// its header records, after the stored bytes' CRC32C check, without decompressing.
Result<uint32_t> ReadContentLength(const ReadableFile& file, const std::string& path,
                                   const Location& loc, uint32_t max_data_size);

}  // namespace dds
