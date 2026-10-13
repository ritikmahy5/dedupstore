#include "dds/repo/blob_reader.h"

#include "dds/common/strings.h"
#include "dds/common/test_hooks.h"
#include "dds/repo/repository.h"

namespace dds {

Result<Location> BlobReader::Locate(const Fingerprint& fp, BlobType type) const {
  const auto loc = repo_.index.Lookup(fp);
  if (!loc.has_value()) {
    return Status::NotFound(StrCat(BlobTypeName(type), " blob ", fp.ToHex(), " is missing"));
  }
  if (loc->type != type) {
    return Status::Corruption(StrCat("blob ", fp.ToHex(), " is a ", BlobTypeName(loc->type),
                                     " blob, but is referenced as a ", BlobTypeName(type)));
  }
  return *loc;
}

Result<std::vector<std::byte>> BlobReader::ReadAt(const Fingerprint& fp, const Location& loc) {
  // Between the caller's guarded lookup and the read: where GC may relocate and retire (6.3).
  DDS_SYNC_POINT("read.after_lookup");
  auto bytes = repo_.containers.Read(loc);
  DDS_TRY(bytes.status());
  if (verify_hash_ && FingerprintOf(loc.type, *bytes) != fp) {
    return Status::Corruption(StrCat(repo_.containers.PathFor(loc.container_id), ": offset ",
                                     loc.offset, ": blob does not hash to its fingerprint ",
                                     fp.ToHex()));
  }
  bytes_read_ += bytes->size();
  return bytes;
}

Result<std::vector<std::byte>> BlobReader::Read(const Fingerprint& fp, BlobType type) {
  auto loc = Locate(fp, type);
  DDS_TRY(loc.status());
  return ReadAt(fp, *loc);
}

Result<Tree> BlobReader::ReadTree(const Fingerprint& fp) {
  auto bytes = Read(fp, BlobType::kTree);
  DDS_TRY(bytes.status());
  auto tree = DecodeTree(*bytes);
  if (!tree.ok()) return tree.status().WithContext(StrCat("tree ", fp.ToHex()));
  return tree;
}

Result<Recipe> BlobReader::ReadRecipe(const Fingerprint& fp) {
  auto bytes = Read(fp, BlobType::kRecipe);
  DDS_TRY(bytes.status());
  auto recipe = DecodeRecipe(*bytes, repo_.config.chunker.max_size);
  if (!recipe.ok()) return recipe.status().WithContext(StrCat("recipe ", fp.ToHex()));
  return recipe;
}

}  // namespace dds
