#include "dds/repo/repository.h"

#include <algorithm>

#include "dds/common/encoding.h"
#include "dds/common/strings.h"
#include "dds/repo/index_checkpoint.h"

namespace dds {

namespace {

constexpr std::string_view kTmpConfigName = "config.tmp";

Status WriteDurably(Fs& fs, const std::string& path, std::string_view text) {
  auto file = fs.CreateNew(path);
  DDS_TRY(file.status());
  DDS_TRY((*file)->Append(AsBytes(text)));
  DDS_TRY((*file)->Sync());
  return (*file)->Close();
}

// ARCHITECTURE.md 7.7 step 1: the target may be absent, empty, or what a crashed init left: the
// three directories, empty except for a possibly partial tmp/config.tmp, and no config (D52).
Status CheckInitTarget(Fs& fs, const RepoLayout& layout) {
  auto exists = fs.Exists(layout.root);
  DDS_TRY(exists.status());
  if (!*exists) return Status::Ok();
  auto names = fs.ListDir(layout.root);
  if (!names.ok())
    return Status::AlreadyExists(StrCat(layout.root, " exists and is not a directory"));
  for (const std::string& name : *names) {
    if (name == "config") {
      return Status::AlreadyExists(StrCat(layout.root, " is already a dedupstore repository"));
    }
    if (name != "tmp" && name != "containers" && name != "snapshots") {
      return Status::AlreadyExists(StrCat(layout.root, " is not empty (it contains ", name, ")"));
    }
    auto entries = fs.ListDir(JoinPath(layout.root, name));
    if (!entries.ok()) {
      return Status::AlreadyExists(StrCat(JoinPath(layout.root, name), " is not a directory"));
    }
    for (const std::string& entry : *entries) {
      if (name != "tmp" || entry != kTmpConfigName) {
        return Status::AlreadyExists(StrCat(JoinPath(layout.root, name), " is not empty"));
      }
    }
  }
  return Status::Ok();
}

// Creates whichever of the repository directories are missing (7.7 step 2). If the root itself
// is created, its parent directory is synced too: otherwise a power loss could drop the new
// repository after Init returned success.
Status CreateDirectories(Fs& fs, const RepoLayout& layout) {
  auto root_exists = fs.Exists(layout.root);
  DDS_TRY(root_exists.status());
  if (!*root_exists) {
    DDS_TRY(fs.CreateDir(layout.root));
    DDS_TRY(fs.SyncDir(ParentDir(layout.root)));
  }
  for (const std::string& dir : {layout.tmp(), layout.containers(), layout.snapshots()}) {
    auto exists = fs.Exists(dir);
    DDS_TRY(exists.status());
    if (!*exists) DDS_TRY(fs.CreateDir(dir));
  }
  return fs.SyncDir(layout.root);
}

// 7.6 step 4: unfinished writes from a crash are garbage.
Status EmptyTmp(Fs& fs, const RepoLayout& layout) {
  auto names = fs.ListDir(layout.tmp());
  DDS_TRY(names.status());
  for (const std::string& name : *names) DDS_TRY(fs.Remove(JoinPath(layout.tmp(), name)));
  return fs.SyncDir(layout.tmp());
}

}  // namespace

RepoInternals::RepoInternals(Fs& fs_in, RepoLayout layout_in, RepoConfig config_in,
                             const Clock& clock_in)
    : fs(fs_in),
      clock(clock_in),
      layout(std::move(layout_in)),
      config(config_in),
      containers(fs_in, layout.containers(), ContainerSet::kDefaultHandleCacheCapacity,
                 config.chunker.max_size),
      snapshots(fs_in, layout) {
  auto made = MakeChunker(config.chunker);
  DDS_CHECK(made.ok());  // the config was validated when it was decoded or created
  chunker = std::move(made).value();
}

ContainerWriter RepoInternals::NewWriter() {
  return ContainerWriter(fs, layout.containers(), layout.tmp(), config.repo_id, containers,
                         config.container_target, config.compression);
}

size_t RepoInternals::PublishSealed(const SealedContainer& sealed, Epoch now) {
  const size_t duplicates = index.Publish(sealed.entries, now);
  containers.MarkPublished(sealed.id, {sealed.payload_bytes, sealed.file_bytes,
                                       static_cast<uint32_t>(sealed.entries.size())});
  return duplicates;
}

Result<RepoConfig> Repository::Init(Fs& fs, const std::string& path, const RepoParams& params,
                                    const Clock& clock) {
  RepoConfig config;
  config.chunker = params.chunker;
  config.container_target = params.container_target;
  config.compression = params.compression;
  config.created_at_ns = clock.WallNanos();
  DDS_TRY(ValidateRepoConfig(config));
  auto repo_id = NewRepoId();
  DDS_TRY(repo_id.status());
  config.repo_id = *repo_id;
  const RepoLayout layout{StripTrailingSlashes(path)};
  DDS_TRY(CheckInitTarget(fs, layout));
  DDS_TRY(CreateDirectories(fs, layout));
  // 7.7 step 3: config through a temporary file; its rename is init's commit point (D52).
  const std::string tmp_config = JoinPath(layout.tmp(), kTmpConfigName);
  auto leftover = fs.Exists(tmp_config);
  DDS_TRY(leftover.status());
  if (*leftover) DDS_TRY(fs.Remove(tmp_config));
  DDS_TRY(WriteDurably(fs, tmp_config, EncodeRepoConfig(config)));
  DDS_TRY(fs.Rename(tmp_config, layout.config()));
  DDS_TRY(fs.SyncDir(layout.root));
  return config;
}

Result<std::unique_ptr<Repository>> Repository::Open(Fs& fs, const std::string& path,
                                                     const ProcessIdentity& self,
                                                     const OpenOptions& options) {
  const Clock& clock = options.clock != nullptr ? *options.clock : SystemClock::Instance();
  const RepoLayout layout{StripTrailingSlashes(path)};
  auto has_config = fs.Exists(layout.config());
  DDS_TRY(has_config.status());
  if (!*has_config) {
    return Status::NotFound(StrCat(layout.root,
                                   ": not a dedupstore repository (no config), or "
                                   "an init that did not finish"));
  }
  auto lock = RepoLock::Acquire(fs, layout, self, clock, options.command);
  DDS_TRY(lock.status());
  // From here, returning an error destroys `lock`, which releases it.
  auto text = ReadWholeFile(fs, layout.config());
  DDS_TRY(text.status());
  auto config = DecodeRepoConfig(AsChars(*text), layout.config());
  DDS_TRY(config.status());
  DDS_TRY(EmptyTmp(fs, layout));
  auto loaded = LoadRepository(fs, layout, *config, clock, options);
  DDS_TRY(loaded.status());
  return std::make_unique<Repository>(Key(), std::move(lock).value(), std::move(loaded->internals),
                                      std::move(loaded->report),
                                      std::move(loaded->checkpoint_covered), options);
}

// Close allocates (it reclaims retired containers), so std::bad_alloc is the one exception that
// could leave this destructor; it would end the process (std::terminate), as running out of memory
// does anyway.
Repository::~Repository() { (void)Close(); }  // NOLINT(bugprone-exception-escape)

Status Repository::Close() {
  if (lock_ == nullptr) return Status::Ok();
  // 4.3 step 6: retired containers are reclaimed at the end of a GC run, at the start of the next,
  // and on close.
  auto reclaimed = internals_->containers.ReclaimRetired(internals_->epochs.OldestReader());
  // After the reclaim, so the checkpoint does not cover containers that are about to go.
  Status checkpointed;
  if (write_checkpoint_ && CheckpointIsStale()) checkpointed = WriteIndexCheckpoint();
  Status released = lock_->Release();
  lock_.reset();
  DDS_TRY(reclaimed.status());
  DDS_TRY(checkpointed);
  return released;
}

Status Repository::WriteIndexCheckpoint() {
  auto covered = dds::WriteIndexCheckpoint(*internals_);
  DDS_TRY(covered.status());
  MutexLock lock(checkpoint_mu_);
  checkpoint_covered_ = std::move(covered).value();
  return Status::Ok();
}

bool Repository::CheckpointIsStale() {
  const std::vector<uint32_t> published = internals_->containers.PublishedNotRetired();
  MutexLock lock(checkpoint_mu_);
  auto missing_from = [](const std::vector<uint32_t>& ids, const std::vector<uint32_t>& sorted) {
    return static_cast<size_t>(std::count_if(ids.begin(), ids.end(), [&](uint32_t id) {
      return !std::binary_search(sorted.begin(), sorted.end(), id);
    }));
  };
  const size_t uncovered = missing_from(published, checkpoint_covered_);
  const size_t deleted = missing_from(checkpoint_covered_, published);
  return uncovered >= checkpoint_stale_containers_ || deleted >= checkpoint_stale_containers_;
}

}  // namespace dds
