#include "support/tree_generator.h"

#include <algorithm>
#include <iterator>

#include "support/random_data.h"

namespace dds::testing {

namespace {

// Sizes where chunking changes behavior at the default parameters (min 2048, avg 8192, max 65536).
constexpr size_t kBoundarySizes[] = {0, 1, 2047, 2048, 2049, 8191, 8192, 65535, 65536, 65537};

constexpr uint32_t kFileModes[] = {0644, 0600, 0755, 0400, 0640};
constexpr uint32_t kDirModes[] = {0755, 0700, 0751, 0555};

const char* const kNonAscii[] = {"\xc3\xa9", "\xe2\x82\xac", "\xe6\x97\xa5", "\xf0\x9f\x93\x81"};

// `bytes` mapped onto the letters a to d: two bits of entropy per byte.
std::vector<std::byte> FourLetters(std::vector<std::byte> bytes) {
  for (std::byte& b : bytes) b = static_cast<std::byte>('a' + (static_cast<int>(b) & 3));
  return bytes;
}

}  // namespace

TreeGenerator::TreeGenerator(uint64_t seed, TreeGenOptions options)
    : rng_(seed), options_(options) {
  for (size_t i = 0; i < 4; ++i) {
    auto block = RandomBytes(rng_(), 64 << 10);
    pool_.push_back(options_.compressible && i % 2 == 0 ? FourLetters(std::move(block)) : block);
  }
}

std::vector<std::byte> TreeGenerator::Content() {
  size_t size;
  switch (Below(10)) {
    case 0:
    case 1:
      size = Below(100);
      break;  // tiny, often empty
    case 2:
    case 3:
    case 4:
      size = 100 + Below(16 << 10);
      break;  // small
    case 5:
    case 6:
      size = (16 << 10) + Below(240 << 10);
      break;  // medium
    case 7:
    case 8:
      size = kBoundarySizes[Below(std::size(kBoundarySizes))];
      break;
    default:
      size = Below(options_.max_file_size + 1);
      break;  // up to the large bound
  }
  size = std::min(size, options_.max_file_size);
  // Half the files are built from slices of the shared pool, so content repeats across files
  // and versions at unaligned offsets, the case content-defined chunking exists for.
  if (Below(2) == 0) {
    auto fresh = RandomBytes(rng_(), size);
    return options_.compressible && Below(2) == 0 ? FourLetters(std::move(fresh)) : fresh;
  }
  std::vector<std::byte> out;
  out.reserve(size);
  while (out.size() < size) {
    const auto& block = pool_[Below(pool_.size())];
    const size_t from = Below(block.size());
    const size_t n = std::min({block.size() - from, size - out.size(), size_t{1} + Below(20000)});
    out.insert(out.end(), block.begin() + static_cast<ptrdiff_t>(from),
               block.begin() + static_cast<ptrdiff_t>(from + n));
  }
  return out;
}

std::string TreeGenerator::Name() {
  std::string name;
  const size_t length = 1 + Below(12);
  while (name.size() < length) {
    const uint64_t pick = Below(20);
    if (pick == 0) {
      name += kNonAscii[Below(std::size(kNonAscii))];
    } else if (pick == 1 && options_.arbitrary_names) {
      char c = static_cast<char>(1 + Below(255));  // any byte but NUL...
      if (c != '/') name += c;                     // ...and '/'
    } else {
      name += static_cast<char>('a' + Below(26));
    }
  }
  if (!options_.arbitrary_names) {
    // Real directories fold case on macOS, so keep generated names in one case.
    std::transform(name.begin(), name.end(), name.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
  }
  if (name == "." || name == "..") name += "x";
  return name;
}

std::string TreeGenerator::RandomDir(const MemTree& tree) {
  std::vector<std::string> dirs = {""};
  for (const auto& [path, node] : tree) {
    if (node.kind == EntryKind::kDirectory) dirs.push_back(path);
  }
  return dirs[Below(dirs.size())];
}

std::string TreeGenerator::FreshPath(const MemTree& tree, const std::string& dir) {
  while (true) {
    std::string path = ChildPath(dir, Name());
    if (tree.count(path) == 0) return path;
  }
}

void TreeGenerator::AddFile(MemTree& tree) {
  MemNode n;
  n.kind = EntryKind::kFile;
  n.mode = kFileModes[Below(std::size(kFileModes))];
  n.mtime_sec = static_cast<int64_t>(Below(4000000000)) - 100000000;  // some before 1970
  n.mtime_nsec = static_cast<uint32_t>(Below(1000000000));
  n.content = Content();
  tree[FreshPath(tree, RandomDir(tree))] = std::move(n);
}

void TreeGenerator::AddDir(MemTree& tree) {
  MemNode n;
  n.kind = EntryKind::kDirectory;
  n.mode = kDirModes[Below(std::size(kDirModes))];
  n.mtime_sec = static_cast<int64_t>(Below(2000000000));
  n.mtime_nsec = static_cast<uint32_t>(Below(1000000000));
  tree[FreshPath(tree, RandomDir(tree))] = std::move(n);
}

void TreeGenerator::AddSymlink(MemTree& tree) {
  MemNode n;
  n.kind = EntryKind::kSymlink;
  n.mode = 0777;
  n.mtime_sec = static_cast<int64_t>(Below(2000000000));
  n.target = Below(2) == 0 ? "../" + Name() : "/abs/" + Name();
  tree[FreshPath(tree, RandomDir(tree))] = std::move(n);
}

MemTree TreeGenerator::NewTree() {
  MemTree tree;
  // Every new tree holds one file of each boundary size and an empty directory with a non-ASCII
  // name, so each seed covers those edge cases rather than finding them by chance.
  MemNode edge;
  edge.kind = EntryKind::kDirectory;
  edge.mode = 0755;
  tree["edge"] = edge;
  tree["edge/empty-\xc3\xa9"] = edge;
  for (size_t size : kBoundarySizes) {
    if (size > options_.max_file_size) continue;
    MemNode file;
    file.kind = EntryKind::kFile;
    file.mode = 0644;
    file.content = RandomBytes(rng_(), size);
    tree["edge/" + std::to_string(size)] = std::move(file);
  }
  for (size_t i = 0; i < options_.files / 5 + 1; ++i) AddDir(tree);
  for (size_t i = 0; i < options_.files; ++i) AddFile(tree);
  for (size_t i = 0; i < options_.files / 10 + 1; ++i) AddSymlink(tree);
  return tree;
}

void TreeGenerator::EditFile(MemNode& file) {
  auto& c = file.content;
  const size_t at = c.empty() ? 0 : Below(c.size() + 1);
  switch (Below(4)) {
    case 0: {  // overwrite a range in place
      const auto fresh = RandomBytes(rng_(), 1 + Below(5000));
      for (size_t i = 0; i < fresh.size() && at + i < c.size(); ++i) c[at + i] = fresh[i];
      break;
    }
    case 1: {  // insert, which shifts everything after it
      const auto fresh = RandomBytes(rng_(), 1 + Below(5000));
      c.insert(c.begin() + static_cast<ptrdiff_t>(at), fresh.begin(), fresh.end());
      break;
    }
    case 2:  // truncate
      c.resize(at);
      break;
    default: {  // append
      const auto fresh = RandomBytes(rng_(), 1 + Below(20000));
      c.insert(c.end(), fresh.begin(), fresh.end());
      break;
    }
  }
  if (c.size() > options_.max_file_size) c.resize(options_.max_file_size);
  file.mtime_sec += 1;
}

void TreeGenerator::RemoveSubtree(MemTree& tree, const std::string& path) {
  const std::string prefix = path + "/";
  for (auto it = tree.lower_bound(prefix); it != tree.end() && it->first.starts_with(prefix);) {
    it = tree.erase(it);
  }
  tree.erase(path);
}

void TreeGenerator::Mutate(MemTree& tree) {
  const size_t changes = 1 + Below(6);
  for (size_t i = 0; i < changes; ++i) {
    std::vector<std::string> files;
    for (const auto& [path, node] : tree) {
      if (node.kind == EntryKind::kFile) files.push_back(path);
    }
    const uint64_t pick = files.empty() ? 5 : Below(10);
    if (pick <= 3) {
      EditFile(tree.at(files[Below(files.size())]));
    } else if (pick == 4) {  // copy: the same content under a new name
      MemNode copy = tree.at(files[Below(files.size())]);
      tree[FreshPath(tree, RandomDir(tree))] = std::move(copy);
    } else if (pick == 5) {
      AddFile(tree);
    } else if (pick == 6) {
      AddDir(tree);
      AddSymlink(tree);
    } else if (pick == 7 && !tree.empty()) {  // delete any entry, a directory with its contents
      auto it = tree.begin();
      std::advance(it, static_cast<ptrdiff_t>(Below(tree.size())));
      RemoveSubtree(tree, std::string(it->first));
    } else if (pick == 8) {  // metadata only
      MemNode& f = tree.at(files[Below(files.size())]);
      f.mode = kFileModes[Below(std::size(kFileModes))];
      f.mtime_nsec = static_cast<uint32_t>(Below(1000000000));
    } else {
      tree.erase(files[Below(files.size())]);
    }
  }
}

}  // namespace dds::testing
