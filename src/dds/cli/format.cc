#include "dds/cli/format.h"

#include <array>
#include <cstdio>
#include <ctime>

namespace dds::cli {

std::string FormatTime(int64_t unix_nanos) {
  // Floor division, so times before 1970 round down to the second they fall in.
  int64_t sec = unix_nanos / 1000000000;
  if (unix_nanos % 1000000000 < 0) --sec;
  return FormatUnixSeconds(sec);
}

std::string FormatUnixSeconds(int64_t sec) {
  const auto t = static_cast<time_t>(sec);
  struct tm utc{};
  if (::gmtime_r(&t, &utc) == nullptr) return std::to_string(sec) + "s";
  std::array<char, 32> buf{};
  const size_t n = std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return std::string(buf.data(), n);
}

std::string FormatBytes(uint64_t bytes) {
  if (bytes < 1024) return std::to_string(bytes) + " B";
  const char* const units[] = {"KiB", "MiB", "GiB", "TiB", "PiB"};
  double value = static_cast<double>(bytes) / 1024;
  size_t unit = 0;
  while (value >= 1024 && unit + 1 < std::size(units)) {
    value /= 1024;
    ++unit;
  }
  std::array<char, 32> buf{};
  std::snprintf(buf.data(), buf.size(), "%.1f %s", value, units[unit]);
  return buf.data();
}

std::string FormatMode(EntryKind kind, uint32_t mode) {
  std::string out = kind == EntryKind::kDirectory ? "d" : kind == EntryKind::kSymlink ? "l" : "-";
  const char rwx[] = "rwx";
  for (int bit = 8; bit >= 0; --bit) out += (mode >> bit) & 1 ? rwx[(8 - bit) % 3] : '-';
  return out;
}

const char* EntryKindName(EntryKind kind) {
  switch (kind) {
    case EntryKind::kFile:
      return "file";
    case EntryKind::kDirectory:
      return "directory";
    case EntryKind::kSymlink:
      return "symlink";
  }
  return "unknown";
}

}  // namespace dds::cli
