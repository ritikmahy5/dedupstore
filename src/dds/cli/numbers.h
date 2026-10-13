#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace dds::cli {

// A byte size: decimal digits, optionally followed by K (×1024) or M (×1024²) (SPEC.md
// section 3). Nothing else is accepted: no lowercase suffix, no "KB", no sign, no spaces, no
// overflow.
std::optional<uint64_t> ParseSize(std::string_view text);

// A positive decimal count (threads, window size): at least 1.
std::optional<uint64_t> ParseCount(std::string_view text);

// A GC threshold in (0, 1] (SPEC.md section 3): digits with an optional fraction, such as "0.5",
// "1", ".9", or "1.0". No sign, exponent, or spaces.
std::optional<double> ParseThreshold(std::string_view text);

// A duration for --gc-interval (SPEC.md section 3): a positive integer followed by s, m, or h.
std::optional<std::chrono::seconds> ParseDuration(std::string_view text);

}  // namespace dds::cli
