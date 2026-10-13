#include "dds/snapshot/mem_node.h"

namespace dds {

std::string ChildPath(const std::string& parent, std::string_view name) {
  if (parent.empty()) return std::string(name);
  std::string out = parent;
  out += '/';
  out += name;
  return out;
}

}  // namespace dds
