#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "dds/chunk/chunker.h"
#include "dds/common/result.h"
#include "dds/store/compression.h"
#include "dds/store/types.h"

namespace dds {

inline constexpr uint32_t kMinContainerTarget = 1u << 20;    // 1 MiB
inline constexpr uint32_t kMaxContainerTarget = 256u << 20;  // 256 MiB
inline constexpr uint32_t kDefaultContainerTarget = 4u << 20;
// Version 2 adds the compression line (D107); version 1 configs still decode, as compression none.
inline constexpr uint32_t kRepoConfigFormatVersion = 2;

// A repository's parameters, fixed at init (D8, D9) and stored in `config`.
struct RepoConfig {
  uint32_t format_version = kRepoConfigFormatVersion;  // decoded from, or to encode
  RepoId repo_id{};
  int64_t created_at_ns = 0;
  ChunkerParams chunker;
  uint32_t container_target = kDefaultContainerTarget;
  CompressionParams compression;  // how new blobs are stored (D107); none in version 1
};

// "none", or "zstd:<level>": the config value and the CLI's --compression argument.
std::string CompressionText(const CompressionParams& compression);
// InvalidArgument unless `text` is "none", "zstd" (level 3, zstd's default), or "zstd:<level>"
// with a canonical decimal level from 1 to 19.
Result<CompressionParams> ParseCompression(std::string_view text);

// InvalidArgument naming the first value outside ON_DISK_FORMAT.md section 3's ranges.
Status ValidateRepoConfig(const RepoConfig& config);

// The canonical text of section 3, ending with its crc32c line. `config` must be valid.
std::string EncodeRepoConfig(const RepoConfig& config);

// Parses `config` text strictly: keys in exactly the documented order, canonical values, and the
// CRC32C of every byte before the crc32c line. A `dedupstore-config` version other than 1 or 2 is
// Incompatible, checked before anything else because a future version may change the rest;
// anything else wrong is Corruption naming `name` and the line.
Result<RepoConfig> DecodeRepoConfig(std::string_view text, std::string_view name = "config");

// 16 random bytes from the operating system (getentropy) for a new repository.
Result<RepoId> NewRepoId();

}  // namespace dds
