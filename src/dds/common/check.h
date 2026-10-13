#pragma once

#include <string_view>

// DDS_CHECK(condition): always-on internal invariant check, in every build including release
// (D6). On failure it prints the file, line, and condition to stderr and aborts.
//
// Use it only for invariants of this program's own logic (a bug if violated). Never use it to
// validate bytes from disk, the network, or the user: those yield Status::Corruption or
// Status::InvalidArgument (D6).
#define DDS_CHECK(condition)                                                        \
  do {                                                                              \
    if (!(condition)) ::dds::internal::CheckFailed(__FILE__, __LINE__, #condition); \
  } while (false)

namespace dds::internal {

// Prints "dds: check failed at <file>:<line>: <condition>" and aborts.
[[noreturn]] void CheckFailed(const char* file, int line, const char* condition);

// Prints "dds: fatal at <file>:<line>: <message>" and aborts. For invariant failures whose
// explanation needs runtime values (for example the lock-order checker).
[[noreturn]] void Fatal(const char* file, int line, std::string_view message);

}  // namespace dds::internal
