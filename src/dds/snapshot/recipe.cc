#include "dds/snapshot/recipe.h"

#include "dds/common/encoding.h"
#include "dds/common/strings.h"

namespace dds {

std::vector<std::byte> EncodeRecipe(const Recipe& recipe) {
  DDS_CHECK(!recipe.refs.empty() && recipe.refs.size() <= kMaxRefsPerRecipe);
  Writer w;
  w.U8(kRecipeFormatVersion);
  w.U32(static_cast<uint32_t>(recipe.refs.size()));
  for (const ChunkRef& ref : recipe.refs) {
    w.Bytes(ref.fp.AsBytes());
    w.U32(ref.length);
  }
  return w.Take();
}

Result<Recipe> DecodeRecipe(std::span<const std::byte> bytes, uint32_t max_chunk_size) {
  Reader r(bytes);
  uint8_t version = 0;
  uint32_t count = 0;
  if (!r.U8(&version)) return Status::Corruption("recipe: empty blob");
  if (version != kRecipeFormatVersion) {
    return Status::Incompatible(StrCat("recipe format version ", version, " is not supported"));
  }
  if (!r.U32(&count)) return Status::Corruption("recipe: truncated header");
  if (count == 0 || count > kMaxRefsPerRecipe || count != r.remaining() / 36 ||
      r.remaining() % 36 != 0) {
    return Status::Corruption(StrCat("recipe: ", count, " refs do not fit ", r.remaining(),
                                     " bytes (1 to 4096 refs of 36 bytes)"));
  }
  Recipe recipe;
  recipe.refs.resize(count);
  for (size_t i = 0; i < count; ++i) {
    std::span<const std::byte> raw;
    DDS_CHECK(r.Bytes(32, &raw) && r.U32(&recipe.refs[i].length));  // sizes checked above
    recipe.refs[i].fp = Fingerprint::FromBytes(raw.first<32>());
    if (recipe.refs[i].length == 0 || recipe.refs[i].length > max_chunk_size) {
      return Status::Corruption(StrCat("recipe ref ", i, ": chunk length ", recipe.refs[i].length,
                                       " outside 1..", max_chunk_size));
    }
  }
  return recipe;
}

}  // namespace dds
