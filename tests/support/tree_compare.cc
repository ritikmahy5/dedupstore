#include "support/tree_compare.h"

#include <functional>
#include <span>

#include "dds/snapshot/source_tree.h"

namespace dds::testing {

std::vector<std::string> DiffMemTrees(const MemTree& expected, const MemTree& actual) {
  std::vector<std::string> diffs;
  for (const auto& [path, want] : expected) {
    if (want.special) {
      if (actual.count(path) > 0) diffs.push_back(path + ": special file was restored");
      continue;
    }
    auto it = actual.find(path);
    if (it == actual.end()) {
      diffs.push_back(path + ": missing");
      continue;
    }
    const MemNode& got = it->second;
    if (got.kind != want.kind) diffs.push_back(path + ": kind differs");
    if (got.content != want.content) diffs.push_back(path + ": content differs");
    if (got.target != want.target) diffs.push_back(path + ": symlink target differs");
    if (want.kind != EntryKind::kSymlink && got.mode != want.mode) {
      diffs.push_back(path + ": mode " + std::to_string(got.mode) +
                      " != " + std::to_string(want.mode));
    }
    if (got.mtime_sec != want.mtime_sec || got.mtime_nsec != want.mtime_nsec) {
      diffs.push_back(path + ": mtime differs");
    }
  }
  for (const auto& [path, node] : actual) {
    if (expected.count(path) == 0) diffs.push_back(path + ": unexpected");
  }
  return diffs;
}

MemTree ReadDirectory(const std::string& root) {
  PosixSourceTree tree(root);
  MemTree out;
  std::function<void(const std::string&)> walk = [&](const std::string& dir) {
    auto entries = tree.ListDir(dir);
    if (!entries.ok()) return;
    for (const SourceEntry& e : *entries) {
      const std::string path = ChildPath(dir, e.name);
      MemNode node;
      node.mode = e.mode;
      node.mtime_sec = e.mtime_sec;
      node.mtime_nsec = e.mtime_nsec;
      if (e.kind == SourceKind::kDirectory) {
        node.kind = EntryKind::kDirectory;
        out[path] = node;
        walk(path);
      } else if (e.kind == SourceKind::kSymlink) {
        node.kind = EntryKind::kSymlink;
        node.mode = 0777;
        node.target = tree.ReadLink(path).value();
        out[path] = node;
      } else if (e.kind == SourceKind::kFile) {
        auto source = tree.OpenFile(path);
        std::vector<std::byte> buf(65536);
        while (true) {
          auto n = (*source)->Read(buf);
          if (!n.ok() || *n == 0) break;
          node.content.insert(node.content.end(), buf.begin(),
                              buf.begin() + static_cast<std::ptrdiff_t>(*n));
        }
        out[path] = node;
      } else {
        node.special = true;
        out[path] = node;
      }
    }
  };
  walk("");
  return out;
}

}  // namespace dds::testing
