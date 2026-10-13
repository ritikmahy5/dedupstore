#include "dds/snapshot/names.h"

namespace dds {

bool IsValidName(std::string_view name) {
  return !name.empty() && name.size() <= 255 && name != "." && name != ".." &&
         name.find('/') == std::string_view::npos && name.find('\0') == std::string_view::npos;
}

bool IsValidSymlinkTarget(std::string_view target) {
  return !target.empty() && target.size() <= 4095 && target.find('\0') == std::string_view::npos;
}

}  // namespace dds
