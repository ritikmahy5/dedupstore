#include "dds/repo/repo_config.h"

#include <sys/random.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <vector>

#include "dds/common/crc32c.h"
#include "dds/common/encoding.h"
#include "dds/common/strings.h"

namespace dds {

namespace {

constexpr int kDefaultZstdLevel = 3;  // zstd's own default (ZSTD_CLEVEL_DEFAULT)

// Version 1 has 12 lines; version 2 adds `compression` before the crc32c line.
size_t LineCount(uint64_t version) { return version >= 2 ? 13 : 12; }

// The lines of a config file, each without its '\n'. nullopt unless every line, including the
// last, ends with exactly one '\n'.
std::optional<std::vector<std::string_view>> SplitLines(std::string_view text) {
  if (text.empty() || text.back() != '\n') return std::nullopt;
  std::vector<std::string_view> lines;
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = text.find('\n', start);
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

class LineParser {
 public:
  LineParser(std::string_view name, const std::vector<std::string_view>& lines)
      : name_(name), lines_(lines) {}

  // The value of line `index` (0-based), which must be "<key>=<value>".
  Result<std::string_view> Value(size_t index, std::string_view key) const {
    const std::string_view line = lines_[index];
    if (!line.starts_with(key) || line.size() <= key.size() || line[key.size()] != '=') {
      return Error(index, StrCat("expected key '", key, "'"));
    }
    return line.substr(key.size() + 1);
  }
  Result<uint32_t> U32(size_t index, std::string_view key) const {
    auto v = Value(index, key);
    DDS_TRY(v.status());
    uint64_t n = 0;
    if (!ParseCanonicalU64(*v, &n) || n > std::numeric_limits<uint32_t>::max()) {
      return Error(index, "not a canonical 32-bit decimal");
    }
    return static_cast<uint32_t>(n);
  }
  Status Error(size_t index, std::string_view what) const {
    return Status::Corruption(StrCat(name_, ": line ", index + 1, ": ", what));
  }

 private:
  std::string_view name_;
  const std::vector<std::string_view>& lines_;
};

Result<RepoConfig> ParseFields(const LineParser& p) {
  RepoConfig c;
  auto repo_id = p.Value(1, "repo_id");
  DDS_TRY(repo_id.status());
  if (!HexDecode(*repo_id, std::as_writable_bytes(std::span(c.repo_id)))) {
    return p.Error(1, "repo_id must be 32 lowercase hex digits");
  }
  auto created = p.Value(2, "created_at_ns");
  DDS_TRY(created.status());
  if (!ParseCanonicalI64(*created, &c.created_at_ns)) return p.Error(2, "bad created_at_ns");
  auto hash = p.Value(3, "hash");
  DDS_TRY(hash.status());
  if (*hash != "blake3-256") return p.Error(3, "hash must be blake3-256");
  auto chunker = p.Value(4, "chunker");
  DDS_TRY(chunker.status());
  if (*chunker == ChunkerKindName(ChunkerKind::kFastCdc)) {
    c.chunker.kind = ChunkerKind::kFastCdc;
  } else if (*chunker == ChunkerKindName(ChunkerKind::kFixed)) {
    c.chunker.kind = ChunkerKind::kFixed;
  } else {
    return p.Error(4, "unknown chunker");
  }
  auto min = p.U32(5, "min_size");
  auto avg = p.U32(6, "avg_size");
  auto max = p.U32(7, "max_size");
  auto level = p.U32(8, "normalization");
  for (const Status& s : {min.status(), avg.status(), max.status(), level.status()}) DDS_TRY(s);
  c.chunker.min_size = *min;
  c.chunker.avg_size = *avg;
  c.chunker.max_size = *max;
  if (*level > 3) return p.Error(8, "normalization must be 0 to 3");
  c.chunker.normalization = static_cast<int>(*level);
  auto seed = p.Value(9, "gear_seed");
  DDS_TRY(seed.status());
  if (!ParseHexU64(*seed, &c.chunker.gear_seed)) return p.Error(9, "bad gear_seed");
  auto target = p.U32(10, "container_target");
  DDS_TRY(target.status());
  c.container_target = *target;
  return c;
}

// Version 2's line 12. Only the canonical spelling is accepted ("zstd:3", never "zstd").
Status ParseCompressionLine(const LineParser& p, RepoConfig& c) {
  auto text = p.Value(11, "compression");
  DDS_TRY(text.status());
  auto parsed = ParseCompression(*text);
  if (!parsed.ok() || CompressionText(*parsed) != *text) {
    return p.Error(11, "compression must be none or zstd:<1-19>");
  }
  c.compression = *parsed;
  return Status::Ok();
}

}  // namespace

std::string CompressionText(const CompressionParams& compression) {
  if (compression.codec == Codec::kNone) return "none";
  return StrCat("zstd:", compression.level);
}

Result<CompressionParams> ParseCompression(std::string_view text) {
  if (text == "none") return CompressionParams{};
  if (text == "zstd") return CompressionParams{Codec::kZstd, kDefaultZstdLevel};
  uint64_t level = 0;
  if (!text.starts_with("zstd:") || !ParseCanonicalU64(text.substr(5), &level) ||
      level < kMinZstdLevel || level > kMaxZstdLevel) {
    return Status::InvalidArgument(
        StrCat("compression '", text, "' must be none, zstd, or zstd:<level> with level 1 to 19"));
  }
  return CompressionParams{Codec::kZstd, static_cast<int>(level)};
}

Status ValidateRepoConfig(const RepoConfig& config) {
  if (config.format_version != 1 && config.format_version != kRepoConfigFormatVersion) {
    return Status::InvalidArgument(StrCat("config format version ", config.format_version));
  }
  const CompressionParams& z = config.compression;
  const bool compression_ok =
      z.codec == Codec::kNone
          ? z.level == 0
          : z.level >= kMinZstdLevel && z.level <= kMaxZstdLevel && config.format_version >= 2;
  if (!compression_ok) {
    return Status::InvalidArgument(StrCat("compression ", CompressionText(z),
                                          " is not allowed in config version ",
                                          config.format_version));
  }
  DDS_TRY(ValidateChunkerParams(config.chunker));
  if (config.container_target < kMinContainerTarget ||
      config.container_target > kMaxContainerTarget) {
    return Status::InvalidArgument(
        StrCat("container_target ", config.container_target, " must be from 1048576 to 268435456"));
  }
  return Status::Ok();
}

std::string EncodeRepoConfig(const RepoConfig& c) {
  DDS_CHECK(ValidateRepoConfig(c).ok());
  std::string body = StrCat(
      "dedupstore-config=", c.format_version, "\n",
      "repo_id=", HexEncode(std::as_bytes(std::span(c.repo_id))), "\n",
      "created_at_ns=", c.created_at_ns, "\n", "hash=blake3-256\n",
      "chunker=", ChunkerKindName(c.chunker.kind), "\n", "min_size=", c.chunker.min_size, "\n",
      "avg_size=", c.chunker.avg_size, "\n", "max_size=", c.chunker.max_size, "\n",
      "normalization=", c.chunker.normalization, "\n", "gear_seed=", HexU64(c.chunker.gear_seed),
      "\n", "container_target=", c.container_target, "\n");
  if (c.format_version >= 2) body += StrCat("compression=", CompressionText(c.compression), "\n");
  return body + "crc32c=" + HexU32(Crc32c(AsBytes(body))) + "\n";
}

Result<RepoConfig> DecodeRepoConfig(std::string_view text, std::string_view name) {
  // The version comes first: a later format may change everything after it.
  const std::string_view version_key = "dedupstore-config=";
  if (!text.starts_with(version_key)) {
    return Status::Corruption(StrCat(name, ": line 1: expected dedupstore-config"));
  }
  const size_t version_end = text.find('\n');
  uint64_t version = 0;
  if (version_end == std::string_view::npos ||
      !ParseCanonicalU64(text.substr(version_key.size(), version_end - version_key.size()),
                         &version)) {
    return Status::Corruption(StrCat(name, ": line 1: bad dedupstore-config value"));
  }
  if (version != 1 && version != kRepoConfigFormatVersion) {
    return Status::Incompatible(StrCat(name, ": repository format version ", version,
                                       " is not supported (this build reads versions 1 and 2)"));
  }
  const size_t line_count = LineCount(version);
  auto lines = SplitLines(text);
  if (!lines || lines->size() != line_count) {
    return Status::Corruption(StrCat(name, ": expected ", line_count, " newline-terminated lines"));
  }
  LineParser p(name, *lines);
  const size_t crc_line = line_count - 1;
  auto crc_text = p.Value(crc_line, "crc32c");
  DDS_TRY(crc_text.status());
  uint32_t stored = 0;
  if (!ParseHexU32(*crc_text, &stored)) {
    return p.Error(crc_line, "crc32c must be 8 lowercase hex digits");
  }
  const size_t body_size = text.size() - (*lines)[crc_line].size() - 1;
  if (Crc32c(AsBytes(text.substr(0, body_size))) != stored)
    return p.Error(crc_line, "CRC mismatch");
  auto config = ParseFields(p);
  DDS_TRY(config.status());
  config->format_version = static_cast<uint32_t>(version);
  if (version >= 2) DDS_TRY(ParseCompressionLine(p, *config));
  if (Status valid = ValidateRepoConfig(*config); !valid.ok()) {
    return Status::Corruption(StrCat(name, ": ", valid.message()));
  }
  return config;
}

Result<RepoId> NewRepoId() {
  RepoId id{};
  if (getentropy(id.data(), id.size()) != 0) {
    return Status::IoError(StrCat("getentropy: ", std::strerror(errno)));
  }
  return id;
}

}  // namespace dds
