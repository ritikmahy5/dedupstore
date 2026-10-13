#include "dds/store/container_writer.h"

#include <limits>

#include "dds/common/crc32c.h"
#include "dds/common/strings.h"
#include "dds/common/test_hooks.h"
#include "dds/store/container_format.h"

namespace dds {

ContainerWriter::ContainerWriter(Fs& fs, std::string containers_dir, std::string tmp_dir,
                                 const RepoId& repo_id, ContainerIdSource& ids,
                                 uint64_t target_bytes, const CompressionParams& compression)
    : fs_(fs),
      containers_dir_(std::move(containers_dir)),
      tmp_dir_(std::move(tmp_dir)),
      repo_id_(repo_id),
      ids_(ids),
      target_bytes_(target_bytes),
      file_(kContainerHeaderSize) {
  if (compression.codec == Codec::kZstd) compressor_.emplace(compression.level);
}

void ContainerWriter::Add(const Fingerprint& fp, BlobType type,
                          std::span<const std::byte> payload) {
  DDS_CHECK(!payload.empty());
  // A container that lists a fingerprint twice fails validation on open (5.5 step 4).
  DDS_CHECK(fingerprints_.insert(fp).second);
  if (id_ == 0) id_ = ids_.AllocateId();
  std::span<const std::byte> stored = payload;
  Codec codec = Codec::kNone;
  if (compressor_.has_value() && compressor_->CompressIfSmaller(payload, scratch_)) {
    stored = scratch_;
    codec = Codec::kZstd;
  }
  const uint64_t offset = file_.size();
  // Offsets are u32; targets are at most 256 MiB and blobs 64 MiB, so this holds by a wide margin.
  DDS_CHECK(offset + stored.size() <= std::numeric_limits<uint32_t>::max());
  file_.insert(file_.end(), stored.begin(), stored.end());
  entries_.push_back({fp,
                      {.container_id = id_,
                       .offset = static_cast<uint32_t>(offset),
                       .length = static_cast<uint32_t>(stored.size()),
                       .crc32c = Crc32c(stored),
                       .type = type,
                       .codec = codec},
                      static_cast<uint32_t>(payload.size())});
}

uint64_t ContainerWriter::FileBytes() const {
  return file_.size() + 4 + kFooterEntrySize * entries_.size() + kContainerTrailerSize;
}

bool ContainerWriter::Full() const { return FileBytes() >= target_bytes_; }

Result<SealedContainer> ContainerWriter::Seal() {
  DDS_CHECK(!Empty());
  const auto header = EncodeContainerHeader(id_, repo_id_);
  std::copy(header.begin(), header.end(), file_.begin());
  const std::vector<std::byte> tail = EncodeContainerTail(header, entries_);
  const std::string name = ContainerFileName(id_);
  const std::string tmp_path = JoinPath(tmp_dir_, name + ".tmp");
  const std::string final_path = JoinPath(containers_dir_, name);

  // 7.2 step 1-2: the whole file under tmp/, then make its data durable.
  auto file = fs_.CreateNew(tmp_path);
  DDS_TRY(file.status());
  DDS_TRY((*file)->Append(file_));
  DDS_TRY((*file)->Append(tail));
  DDS_TRY((*file)->Sync());
  DDS_TRY((*file)->Close());
  // Step 3: never replace a container. Open sets the next id above every existing name, so a file
  // here means the repository is inconsistent; failing keeps its bytes.
  auto exists = fs_.Exists(final_path);
  DDS_TRY(exists.status());
  if (*exists) {
    return Status::Corruption(
        StrCat(final_path, ": a container with this id already exists; refusing to replace it"));
  }
  DDS_TRY(fs_.Rename(tmp_path, final_path));
  // Step 4: the rename is durable only once the target directory is synced (assumption A2).
  if (!DDS_MUTATION("skip_container_dir_sync")) DDS_TRY(fs_.SyncDir(containers_dir_));

  SealedContainer sealed{id_, file_.size() - kContainerHeaderSize, file_.size() + tail.size(),
                         std::move(entries_)};
  id_ = 0;
  file_.assign(kContainerHeaderSize, std::byte{0});
  entries_.clear();
  fingerprints_.clear();
  return sealed;
}

}  // namespace dds
