// dds backup <repo> <path> [--threads N] [--split-size B] [--tag TEXT] [--parent auto|none|ID]
//            [--json]

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <utility>

#include "dds/cli/command.h"
#include "dds/cli/format.h"
#include "dds/cli/json.h"
#include "dds/cli/numbers.h"
#include "dds/cli/repo_access.h"
#include "dds/common/clock.h"
#include "dds/common/strings.h"
#include "dds/snapshot/source_tree.h"

namespace dds::cli {

namespace {

constexpr char kUsage[] =
    "dds backup <repo> <path> [--threads N] [--split-size B] [--tag TEXT] [--parent auto|none|ID] "
    "[--json]";

std::string HostName() {
  std::array<char, 256> buf{};
  if (::gethostname(buf.data(), buf.size() - 1) != 0) return "";
  return buf.data();
}

void PrintJson(const BackupResult& b, double seconds) {
  const Manifest& m = b.manifest;
  JsonWriter w;
  w.BeginObject();
  w.Key("id");
  w.String(SnapshotIdToHex(b.id));
  w.Key("created_at_ns");
  w.Int(m.created_at_ns);
  w.Key("source_path");
  w.String(m.source_path);
  w.Key("tag");
  w.String(m.tag);
  w.Key("files");
  w.Uint(m.file_count);
  w.Key("directories");
  w.Uint(m.directory_count);
  w.Key("symlinks");
  w.Uint(m.symlink_count);
  w.Key("logical_bytes");
  w.Uint(m.logical_bytes);
  w.Key("blobs_written");
  w.Uint(b.ingest.blobs_written);
  w.Key("bytes_written");
  w.Uint(b.ingest.bytes_written);
  w.Key("dedup_hits");
  w.Uint(b.ingest.dedup_hits);
  w.Key("duplicates_published");
  w.Uint(b.ingest.duplicates_published);
  w.Key("containers_sealed");
  w.Uint(b.ingest.containers_sealed);
  w.Key("skipped_special");
  w.Uint(b.skipped_special);
  w.Key("changed_while_reading");
  w.Uint(b.changed_while_reading);
  w.Key("parent");
  if (b.parent.has_value()) {
    w.String(SnapshotIdToHex(*b.parent));
  } else {
    w.Null();
  }
  w.Key("files_reused");
  w.Uint(b.files_reused);
  w.Key("bytes_reused");
  w.Uint(b.bytes_reused);
  w.Key("files_split");
  w.Uint(b.files_split);
  w.Key("seconds");
  w.Double(seconds);
  // Seconds per component (BENCHMARKS.md E3); the worker steps are summed over the workers.
  const BackupTimings& t = b.timings;
  w.Key("timings");
  w.BeginObject();
  for (const auto& [name, ns] :
       {std::pair{"walk", t.walk_ns}, std::pair{"read_chunk", t.read_chunk_ns},
        std::pair{"hash", t.hash_ns}, std::pair{"store", t.store_ns},
        std::pair{"reuse", t.reuse_ns}, std::pair{"trees", t.trees_ns},
        std::pair{"commit", t.commit_ns}}) {
    w.Key(name);
    w.Double(static_cast<double>(ns) / 1e9);
  }
  w.EndObject();
  w.EndObject();
  std::printf("%s\n", w.Take().c_str());
}

void PrintHuman(const BackupResult& b, double seconds) {
  const Manifest& m = b.manifest;
  std::fprintf(stderr, "snapshot %s: %llu files, %llu directories, %llu symlinks, %s in %.2f s\n",
               SnapshotIdToHex(b.id).c_str(), static_cast<unsigned long long>(m.file_count),
               static_cast<unsigned long long>(m.directory_count),
               static_cast<unsigned long long>(m.symlink_count),
               FormatBytes(m.logical_bytes).c_str(), seconds);
  if (b.parent.has_value()) {
    std::fprintf(stderr, "  parent %s: %llu unchanged files (%s) not read\n",
                 SnapshotIdToHex(*b.parent).c_str(),
                 static_cast<unsigned long long>(b.files_reused),
                 FormatBytes(b.bytes_reused).c_str());
  }
  std::fprintf(stderr, "  stored %llu new blobs (%s); %llu references deduplicated\n",
               static_cast<unsigned long long>(b.ingest.blobs_written),
               FormatBytes(b.ingest.bytes_written).c_str(),
               static_cast<unsigned long long>(b.ingest.dedup_hits));
  if (b.skipped_special > 0 || b.changed_while_reading > 0) {
    std::fprintf(stderr, "  warnings: %llu special files skipped, %llu files changed while read\n",
                 static_cast<unsigned long long>(b.skipped_special),
                 static_cast<unsigned long long>(b.changed_while_reading));
  }
  // The id alone on stdout, so `id=$(dds backup repo dir)` works (D70).
  std::printf("%s\n", SnapshotIdToHex(b.id).c_str());
}

// --parent (D106): auto picks the newest snapshot this repository created from the same source
// path and host, so a nightly backup of one directory skips what has not changed since the last.
Result<std::optional<SnapshotId>> ChooseParent(Repository& repo, const std::string& choice,
                                               const BackupOptions& options) {
  if (choice == "none") return std::optional<SnapshotId>();
  if (choice != "auto") {
    auto id = repo.ResolveSnapshot(choice);
    DDS_TRY(id.status());
    return std::optional<SnapshotId>(*id);
  }
  auto snapshots = repo.ListSnapshots();
  DDS_TRY(snapshots.status());
  std::optional<SnapshotId> newest;
  for (const SnapshotInfo& s : *snapshots) {  // oldest first, so the last match is the newest
    const Manifest& m = s.manifest;
    if (m.origin_repo_id == repo.config().repo_id && m.source_path == options.source_path &&
        m.host == options.host) {
      newest = s.id;
    }
  }
  return newest;
}

int Run(const ParsedArgs& args) {
  BackupOptions options;
  options.threads = std::max(1u, std::thread::hardware_concurrency());
  if (auto text = args.Get("threads")) {
    auto n = ParseCount(*text);
    if (!n || *n > 1024)
      return UsageError(kUsage, StrCat("--threads must be 1 to 1024, got '", *text, "'"));
    options.threads = static_cast<size_t>(*n);
  }
  if (auto text = args.Get("split-size")) {
    auto bytes = ParseSize(*text);
    if (!bytes) return UsageError(kUsage, StrCat("--split-size must be a size, got '", *text, "'"));
    options.split_file_bytes = *bytes == 0 ? UINT64_MAX : *bytes;  // 0: never split (D109)
  }
  options.tag = args.Get("tag").value_or("");  // the library checks its length (exit 8)
  const std::string& source_path = args.positional[1];
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(source_path, ec);
  options.source_path = ec ? source_path : absolute.lexically_normal().string();
  options.host = HostName();

  auto repo = OpenRepository(args.positional[0], "dds backup");
  if (!repo.ok()) return ReportError(repo.status());
  auto parent = ChooseParent(**repo, args.Get("parent").value_or("auto"), options);
  if (!parent.ok()) return ReportError(parent.status());
  options.parent = *parent;
  PosixSourceTree source(source_path);
  const Clock& clock = SystemClock::Instance();
  const int64_t start = clock.MonotonicNanos();
  auto result = (*repo)->Backup(source, options);
  const double seconds = static_cast<double>(clock.MonotonicNanos() - start) / 1e9;
  CloseRepository(**repo);
  if (!result.ok()) return ReportError(result.status());
  if (args.Has("json")) {
    PrintJson(*result, seconds);
  } else {
    PrintHuman(*result, seconds);
  }
  return kExitOk;
}

}  // namespace

Command MakeBackupCommand() {
  Command cmd;
  cmd.name = "backup";
  cmd.usage = kUsage;
  cmd.summary = "snapshot a directory tree";
  cmd.flags = {
      {"threads", true}, {"split-size", true}, {"tag", true}, {"parent", true}, {"json", false}};
  cmd.min_positional = 2;
  cmd.max_positional = 2;
  cmd.run = Run;
  return cmd;
}

}  // namespace dds::cli
