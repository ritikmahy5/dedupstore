#include "dds/repo/verifier.h"

#include <optional>
#include <unordered_map>
#include <vector>

#include "dds/common/strings.h"
#include "dds/repo/dag_walker.h"
#include "dds/repo/repository.h"
#include "dds/store/container_format.h"
#include "dds/store/container_reader.h"

namespace dds {

namespace {

// For each valid container, how many index entries point at one of its footer's entries exactly.
using MatchCounts = std::unordered_map<uint32_t, uint64_t>;

// The index entries that point into container `id` must each equal one of its footer entries.
uint64_t MatchFooter(RepoInternals& repo, uint32_t id, const std::vector<SealedEntry>& footer,
                     VerifyReport& report) {
  uint64_t matched = 0;
  for (const SealedEntry& e : footer) {
    const std::optional<Location> loc = repo.index.Lookup(e.fp);
    if (!loc.has_value() || loc->container_id != id) continue;
    if (*loc == e.loc) {
      ++matched;
    } else {
      report.errors.push_back(
          StrCat("index entry for ", e.fp.ToHex(), " differs from container ", id, "'s footer"));
    }
  }
  return matched;
}

// Check 2b (D104): no index entry points into a valid container without matching its footer, as a
// wrong index checkpoint could make one. Only containers checked above are counted, so containers
// published or relocated into during verify cannot be miscounted.
void CheckIndexAgainstFooters(RepoInternals& repo, const MatchCounts& matched,
                              VerifyReport& report) {
  std::unordered_map<uint32_t, uint64_t> pointing;
  for (size_t shard = 0; shard < ChunkIndex::kShardCount; ++shard) {
    repo.index.ForEachInShard(shard, [&](const IndexEntry& e) {
      if (matched.count(e.container_id) != 0) ++pointing[e.container_id];
    });
  }
  for (const auto& [id, count] : pointing) {
    if (count > matched.at(id)) {
      report.errors.push_back(StrCat(count - matched.at(id), " index entries point into container ",
                                     id, " but match no entry of its footer"));
    }
  }
}

// Check 2: every container file's header, footer, and trailer, read afresh rather than trusted
// from open, so damage since open is found too.
Status CheckContainers(RepoInternals& repo, VerifyReport& report, MatchCounts& matched) {
  auto names = repo.fs.ListDir(repo.layout.containers());
  DDS_TRY(names.status());
  for (const std::string& name : *names) {
    const auto id = ParseContainerFileName(name);
    if (!id.has_value()) {
      report.warnings.push_back(
          StrCat(JoinPath(repo.layout.containers(), name), ": not a container file name; ignored"));
      continue;
    }
    auto meta = ReadContainerMetadata(repo.fs, repo.containers.PathFor(*id), *id,
                                      repo.config.repo_id, repo.config.chunker.max_size);
    if (meta.ok()) {
      ++report.containers;
      matched[*id] = MatchFooter(repo, *id, meta->entries, report);
    } else {
      report.errors.push_back(meta.status().ToString());
    }
  }
  return Status::Ok();
}

// Checks 1 and 3-5: every manifest, then every snapshot's DAG.
Status CheckSnapshots(RepoInternals& repo, DagWalker& walker, VerifyReport& report) {
  auto listing = repo.snapshots.List();
  DDS_TRY(listing.status());
  for (const std::string& name : listing->ignored) {
    report.warnings.push_back(
        StrCat(JoinPath(repo.layout.snapshots(), name), ": not a snapshot file name; ignored"));
  }
  for (const SnapshotId& id : listing->ids) {
    const std::string label = StrCat("snapshot ", SnapshotIdToHex(id));
    auto manifest = repo.snapshots.Read(id);
    if (manifest.status().code() == StatusCode::kNotFound) continue;  // removed since listing
    ++report.snapshots;
    if (!manifest.ok()) {
      report.errors.push_back(StrCat(label, ": ", manifest.status().ToString()));
      continue;
    }
    std::vector<std::string> problems;
    walker.Walk(manifest->root, problems);
    for (const std::string& p : problems) report.errors.push_back(StrCat(label, ": ", p));
  }
  return Status::Ok();
}

}  // namespace

Result<VerifyReport> RunVerify(RepoInternals& repo, const VerifyOptions& options) {
  VerifyReport report;
  MatchCounts matched;
  DDS_TRY(CheckContainers(repo, report, matched));
  CheckIndexAgainstFooters(repo, matched, report);
  DagWalker walker(repo, options.deep);
  DDS_TRY(CheckSnapshots(repo, walker, report));
  const auto& reachable = walker.reachable();
  report.reachable_blobs = reachable.size();
  report.bytes_hashed = walker.bytes_hashed();
  for (size_t shard = 0; shard < ChunkIndex::kShardCount; ++shard) {
    repo.index.ForEachInShard(shard, [&](const IndexEntry& e) {
      if (reachable.count(e.fp) == 0) {
        ++report.unreachable_blobs;
        report.unreachable_bytes += e.length;
      }
    });
  }
  // D32: every published footer entry beyond the first copy of its fingerprint.
  report.duplicate_copies = repo.containers.PublishedTotals().entries - repo.index.Size();
  return report;
}

}  // namespace dds
