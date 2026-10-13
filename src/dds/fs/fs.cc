#include "dds/fs/fs.h"

#include <string>

namespace dds {

std::string JoinPath(std::string_view dir, std::string_view name) {
  std::string out(dir);
  if (out.empty() || out.back() != '/') out += '/';
  out += name;
  return out;
}

std::string ParentDir(std::string_view path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string_view::npos) return ".";
  if (slash == 0) return "/";
  return std::string(path.substr(0, slash));
}

std::string StripTrailingSlashes(std::string path) {
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  return path;
}

Result<std::vector<std::byte>> ReadWholeFile(Fs& fs, const std::string& path) {
  auto file = fs.OpenRead(path);
  DDS_TRY(file.status());
  auto size = (*file)->Size();
  DDS_TRY(size.status());
  std::vector<std::byte> data(static_cast<size_t>(*size));
  DDS_TRY((*file)->ReadAt(0, data));
  return data;
}

}  // namespace dds
