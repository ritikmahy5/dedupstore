#include "dds/common/check.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace dds::internal {

void CheckFailed(const char* file, int line, const char* condition) {
  std::fprintf(stderr, "dds: check failed at %s:%d: %s\n", file, line, condition);
  std::fflush(stderr);
  std::abort();
}

void Fatal(const char* file, int line, std::string_view message) {
  std::string text(message);
  std::fprintf(stderr, "dds: fatal at %s:%d: %s\n", file, line, text.c_str());
  std::fflush(stderr);
  std::abort();
}

}  // namespace dds::internal
