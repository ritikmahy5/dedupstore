#include "dds/cli/numbers.h"

#include <cstdlib>
#include <limits>
#include <string>

#include "dds/common/strings.h"

namespace dds::cli {

std::optional<uint64_t> ParseSize(std::string_view text) {
  uint64_t multiplier = 1;
  if (!text.empty() && (text.back() == 'K' || text.back() == 'M')) {
    multiplier = text.back() == 'K' ? 1024 : 1024 * 1024;
    text.remove_suffix(1);
  }
  uint64_t value = 0;
  if (!ParseCanonicalU64(text, &value)) return std::nullopt;
  if (value > std::numeric_limits<uint64_t>::max() / multiplier) return std::nullopt;
  return value * multiplier;
}

std::optional<uint64_t> ParseCount(std::string_view text) {
  uint64_t value = 0;
  if (!ParseCanonicalU64(text, &value) || value == 0) return std::nullopt;
  return value;
}

std::optional<double> ParseThreshold(std::string_view text) {
  // The shape is checked here, so strtod only ever sees digits and one optional point, which it
  // reads the same in every locale that uses '.' (the C locale, which dds never changes).
  size_t digits = 0;
  size_t points = 0;
  for (char c : text) {
    if (c >= '0' && c <= '9') {
      ++digits;
    } else if (c == '.') {
      ++points;
    } else {
      return std::nullopt;
    }
  }
  if (digits == 0 || points > 1 || text.size() > 32) return std::nullopt;
  const std::string copy(text);  // strtod needs a NUL-terminated string
  const double value = std::strtod(copy.c_str(), nullptr);
  if (!(value > 0 && value <= 1)) return std::nullopt;
  return value;
}

std::optional<std::chrono::seconds> ParseDuration(std::string_view text) {
  if (text.size() < 2) return std::nullopt;
  uint64_t multiplier = 0;
  switch (text.back()) {
    case 's':
      multiplier = 1;
      break;
    case 'm':
      multiplier = 60;
      break;
    case 'h':
      multiplier = 3600;
      break;
    default:
      return std::nullopt;
  }
  uint64_t count = 0;
  // At most 10 years: anything longer is a typo, and the product stays far from overflow.
  if (!ParseCanonicalU64(text.substr(0, text.size() - 1), &count) || count == 0 ||
      count * multiplier > 315360000) {
    return std::nullopt;
  }
  return std::chrono::seconds(static_cast<int64_t>(count * multiplier));
}

}  // namespace dds::cli
