#include "dds/common/encoding.h"

#include <array>

namespace dds {

void Writer::U16(uint16_t v) {
  std::array<std::byte, 2> b{};
  StoreU16(b.data(), v);
  Bytes(b);
}

void Writer::U32(uint32_t v) {
  std::array<std::byte, 4> b{};
  StoreU32(b.data(), v);
  Bytes(b);
}

void Writer::U64(uint64_t v) {
  std::array<std::byte, 8> b{};
  StoreU64(b.data(), v);
  Bytes(b);
}

void Writer::Str16(std::string_view s) {
  DDS_CHECK(s.size() <= 0xFFFF);
  U16(static_cast<uint16_t>(s.size()));
  Bytes(AsBytes(s));
}

bool Reader::U8(uint8_t* out) {
  if (remaining() < 1) return false;
  *out = static_cast<uint8_t>(data_[pos_]);
  pos_ += 1;
  return true;
}

bool Reader::U16(uint16_t* out) {
  if (remaining() < 2) return false;
  *out = LoadU16(data_.data() + pos_);
  pos_ += 2;
  return true;
}

bool Reader::U32(uint32_t* out) {
  if (remaining() < 4) return false;
  *out = LoadU32(data_.data() + pos_);
  pos_ += 4;
  return true;
}

bool Reader::U64(uint64_t* out) {
  if (remaining() < 8) return false;
  *out = LoadU64(data_.data() + pos_);
  pos_ += 8;
  return true;
}

bool Reader::I64(int64_t* out) {
  uint64_t v = 0;
  if (!U64(&v)) return false;
  *out = static_cast<int64_t>(v);  // modular conversion: two's complement (C++20)
  return true;
}

bool Reader::Bytes(size_t n, std::span<const std::byte>* out) {
  // Compare against remaining() rather than computing pos_ + n, which could overflow.
  if (n > remaining()) return false;
  *out = data_.subspan(pos_, n);
  pos_ += n;
  return true;
}

bool Reader::Str16(std::string* out) {
  const size_t start = pos_;
  uint16_t len = 0;
  if (!U16(&len)) return false;
  std::span<const std::byte> bytes;
  if (!Bytes(len, &bytes)) {
    pos_ = start;  // a failed read never advances
    return false;
  }
  out->assign(AsChars(bytes));
  return true;
}

}  // namespace dds
