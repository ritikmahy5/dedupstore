#include "dds/common/strings.h"

#include <array>
#include <limits>

namespace dds {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

// Value of a lowercase hex digit, or -1.
int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool ParseFixedHex(std::string_view hex, size_t digits, uint64_t* out) {
  if (hex.size() != digits) return false;
  uint64_t v = 0;
  for (char c : hex) {
    int d = HexValue(c);
    if (d < 0) return false;
    v = (v << 4) | static_cast<uint64_t>(d);
  }
  *out = v;
  return true;
}

}  // namespace

std::string HexEncode(std::span<const std::byte> bytes) {
  std::string out;
  out.reserve(bytes.size() * 2);
  for (std::byte b : bytes) {
    const auto v = static_cast<unsigned>(b);
    out.push_back(kHexDigits[v >> 4]);
    out.push_back(kHexDigits[v & 0xF]);
  }
  return out;
}

bool HexDecode(std::string_view hex, std::span<std::byte> out) {
  if (hex.size() != out.size() * 2) return false;
  for (size_t i = 0; i < out.size(); ++i) {
    const int hi = HexValue(hex[2 * i]);
    const int lo = HexValue(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<std::byte>((hi << 4) | lo);
  }
  return true;
}

std::string HexU32(uint32_t value) {
  std::string out(8, '0');
  for (int i = 7; i >= 0; --i, value >>= 4) out[static_cast<size_t>(i)] = kHexDigits[value & 0xF];
  return out;
}

std::string HexU64(uint64_t value) {
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i, value >>= 4) out[static_cast<size_t>(i)] = kHexDigits[value & 0xF];
  return out;
}

bool ParseHexU32(std::string_view hex, uint32_t* out) {
  uint64_t v = 0;
  if (!ParseFixedHex(hex, 8, &v)) return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

bool ParseHexU64(std::string_view hex, uint64_t* out) { return ParseFixedHex(hex, 16, out); }

bool ParseCanonicalU64(std::string_view text, uint64_t* out) {
  if (text.empty()) return false;
  if (text.size() > 1 && text[0] == '0') return false;  // "0" has exactly one spelling
  uint64_t v = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    const auto digit = static_cast<uint64_t>(c - '0');
    if (v > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;  // overflow
    v = v * 10 + digit;
  }
  *out = v;
  return true;
}

bool ParseCanonicalI64(std::string_view text, int64_t* out) {
  const bool negative = !text.empty() && text[0] == '-';
  if (negative) text.remove_prefix(1);
  uint64_t magnitude = 0;
  if (!ParseCanonicalU64(text, &magnitude)) return false;
  // |INT64_MIN| = INT64_MAX + 1, which only a negative value may reach.
  constexpr auto kMaxPositive = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  if (negative) {
    if (magnitude == 0 || magnitude > kMaxPositive + 1) return false;
    // Negate in unsigned arithmetic (well defined), then convert: C++20 defines the conversion
    // of an out-of-range unsigned value as modular, which yields the two's complement value.
    *out = static_cast<int64_t>(~magnitude + 1);
  } else {
    if (magnitude > kMaxPositive) return false;
    *out = static_cast<int64_t>(magnitude);
  }
  return true;
}

}  // namespace dds
