#include "dds/repo/dag_walker.h"

#include <algorithm>

#include "dds/common/strings.h"
#include "dds/repo/repository.h"
#include "dds/snapshot/mem_node.h"

namespace dds {

void DagWalker::Walk(const Fingerprint& root, std::vector<std::string>& problems) {
  for (const Problem& p : CheckTree(root)) {
    problems.push_back(StrCat(p.path.empty() ? "/" : p.path, ": ", p.message));
  }
}

Result<Location> DagWalker::Reach(const Fingerprint& fp, BlobType type) {
  auto loc = reader_.Locate(fp, type);
  if (loc.ok()) reachable_.emplace(fp, *loc);
  return loc;
}

const std::vector<DagWalker::Problem>& DagWalker::CheckTree(const Fingerprint& fp) {
  if (auto known = trees_.find(fp); known != trees_.end()) return known->second;
  std::vector<Problem> found = CheckEntries(fp);
  return trees_.emplace(fp, std::move(found)).first->second;
}

Result<std::vector<std::byte>> DagWalker::GuardedRead(const Fingerprint& fp, BlobType type) {
  ReadGuard guard(repo_.epochs);  // one blob: lookup, then read (6.3)
  auto loc = Reach(fp, type);
  DDS_TRY(loc.status());
  return reader_.ReadAt(fp, *loc);
}

std::vector<DagWalker::Problem> DagWalker::CheckEntries(const Fingerprint& fp) {
  auto bytes = GuardedRead(fp, BlobType::kTree);
  if (!bytes.ok()) return {{"", bytes.status().ToString()}};
  auto tree = DecodeTree(*bytes);
  if (!tree.ok()) return {{"", tree.status().WithContext(StrCat("tree ", fp.ToHex())).ToString()}};

  std::vector<Problem> found;
  ancestors_.push_back(fp);
  for (const TreeEntry& e : tree->entries) {
    if (e.kind == EntryKind::kDirectory) {
      if (std::find(ancestors_.begin(), ancestors_.end(), e.subtree) != ancestors_.end()) {
        found.push_back(
            {e.name, StrCat("Corruption: tree ", e.subtree.ToHex(), " contains itself")});
        continue;
      }
      for (const Problem& p : CheckTree(e.subtree)) {
        found.push_back({p.path.empty() ? e.name : ChildPath(e.name, p.path), p.message});
      }
    } else if (e.kind == EntryKind::kFile) {
      for (std::string& message : CheckFile(e)) found.push_back({e.name, std::move(message)});
    }
  }
  ancestors_.pop_back();
  return found;
}

std::vector<std::string> DagWalker::CheckFile(const TreeEntry& e) {
  std::vector<std::string> found;
  uint64_t total = 0;
  bool complete = true;
  for (const Fingerprint& piece_fp : e.pieces) {
    const PieceResult& piece = CheckPiece(piece_fp);
    found.insert(found.end(), piece.problems.begin(), piece.problems.end());
    complete = complete && piece.bytes.has_value();
    total += piece.bytes.value_or(0);
  }
  // ON_DISK_FORMAT.md section 6.
  if (complete && total != e.size) {
    found.push_back(
        StrCat("Corruption: chunks add up to ", total, " bytes, the tree says ", e.size));
  }
  return found;
}

const DagWalker::PieceResult& DagWalker::CheckPiece(const Fingerprint& fp) {
  if (auto known = pieces_.find(fp); known != pieces_.end()) return known->second;
  PieceResult result = ReadPiece(fp);
  return pieces_.emplace(fp, std::move(result)).first->second;
}

DagWalker::PieceResult DagWalker::ReadPiece(const Fingerprint& fp) {
  PieceResult result;
  auto bytes = GuardedRead(fp, BlobType::kRecipe);
  if (!bytes.ok()) return {{bytes.status().ToString()}, std::nullopt};
  auto recipe = DecodeRecipe(*bytes, repo_.config.chunker.max_size);
  if (!recipe.ok()) {
    return {{recipe.status().WithContext(StrCat("recipe ", fp.ToHex())).ToString()}, std::nullopt};
  }
  uint64_t total = 0;
  for (const ChunkRef& ref : recipe->refs) {
    total += ref.length;
    if (const auto& problem = CheckData(ref.fp)) {
      result.problems.push_back(*problem);
      continue;
    }
    const Location& loc = reachable_.at(ref.fp);
    if (data_content_.emplace(ref.fp, ref.length).second) {
      data_content_bytes_ += ref.length;
      data_stored_bytes_ += loc.length;
    }
    // A blob stored as content has its content's length. A compressed one (D107) does not: its
    // content's length is checked when it is read, and a deep walk reads and hashes it.
    if (loc.codec == Codec::kNone && loc.length != ref.length) {
      // The recipe is wrong, not the blob: a fingerprint fixes its content's length.
      result.problems.push_back(StrCat("Corruption: recipe ", fp.ToHex(), " says chunk ",
                                       ref.fp.ToHex(), " is ", ref.length, " bytes; it is ",
                                       loc.length));
    }
  }
  result.bytes = total;
  return result;
}

const std::optional<std::string>& DagWalker::CheckData(const Fingerprint& fp) {
  if (auto known = data_.find(fp); known != data_.end()) return known->second;
  std::optional<std::string> problem;
  if (deep_) {
    auto bytes = GuardedRead(fp, BlobType::kData);
    if (!bytes.ok()) problem = bytes.status().ToString();
  } else if (auto loc = Reach(fp, BlobType::kData); !loc.ok()) {
    problem = loc.status().ToString();  // a lookup without a read needs no guard
  }
  return data_.emplace(fp, std::move(problem)).first->second;
}

}  // namespace dds
