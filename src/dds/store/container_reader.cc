#include "dds/store/container_reader.h"

#include "dds/common/crc32c.h"
#include "dds/common/strings.h"

namespace dds {

Result<ContainerMetadata> ReadContainerMetadata(Fs& fs, const std::string& path,
                                                uint32_t expected_id, const RepoId& repo_id,
                                                uint32_t max_data_size) {
  auto file = fs.OpenRead(path);
  DDS_TRY(file.status());
  auto size = (*file)->Size();
  DDS_TRY(size.status());
  if (*size < kMinContainerFileSize) {
    return Status::Corruption(
        StrCat(path, ": offset 0: file is ", *size, " bytes, smaller than any container"));
  }
  // The trailer says where the footer is, so it is read first; its sizes are checked against the
  // file size before any footer-sized buffer is allocated.
  std::vector<std::byte> trailer_bytes(kContainerTrailerSize);
  DDS_TRY((*file)->ReadAt(*size - kContainerTrailerSize, trailer_bytes));
  auto trailer = ParseContainerTrailer(trailer_bytes, *size, path);
  DDS_TRY(trailer.status());
  std::vector<std::byte> header(kContainerHeaderSize);
  DDS_TRY((*file)->ReadAt(0, header));
  std::vector<std::byte> footer(trailer->footer_length);
  DDS_TRY((*file)->ReadAt(trailer->footer_offset, footer));
  return DecodeContainerParts(header, footer, *trailer, *size, path, expected_id, repo_id,
                              max_data_size);
}

namespace {

// The stored bytes at `loc`, after their CRC32C check (D26).
Result<std::vector<std::byte>> ReadStored(const ReadableFile& file, const std::string& path,
                                          const Location& loc) {
  std::vector<std::byte> stored(loc.length);
  DDS_TRY(file.ReadAt(loc.offset, stored));
  const uint32_t crc = Crc32c(stored);
  if (crc != loc.crc32c) {
    return Status::Corruption(StrCat(path, ": offset ", loc.offset, ": payload CRC32C ",
                                     HexU32(crc), " differs from the footer's ",
                                     HexU32(loc.crc32c)));
  }
  return stored;
}

}  // namespace

Result<uint32_t> ReadContentLength(const ReadableFile& file, const std::string& path,
                                   const Location& loc, uint32_t max_data_size) {
  if (loc.codec == Codec::kNone) return loc.length;
  auto stored = ReadStored(file, path, loc);
  DDS_TRY(stored.status());
  const uint32_t limit = loc.type == BlobType::kData ? max_data_size : kMaxMetadataBlobBytes;
  auto size = ZstdContentSize(*stored, limit);
  if (!size.ok()) return size.status().WithContext(StrCat(path, ": offset ", loc.offset));
  return size;
}

Result<std::vector<std::byte>> ReadBlob(const ReadableFile& file, const std::string& path,
                                        const Location& loc, uint32_t max_data_size) {
  auto read = ReadStored(file, path, loc);
  DDS_TRY(read.status());
  std::vector<std::byte> stored = std::move(read).value();
  if (loc.codec == Codec::kNone) return stored;
  const uint32_t limit = loc.type == BlobType::kData ? max_data_size : kMaxMetadataBlobBytes;
  auto content = DecompressZstd(stored, limit);
  if (!content.ok()) return content.status().WithContext(StrCat(path, ": offset ", loc.offset));
  return content;
}

}  // namespace dds
