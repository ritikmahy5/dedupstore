#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dds/common/check.h"

namespace dds {

// Little-endian encoding (D27). Every integer stored on disk or sent on the wire goes through
// these helpers. Explicit shifts produce the bytes ON_DISK_FORMAT.md specifies on every platform;
// copying a struct with memcpy would expose compiler-chosen padding and the host byte order.

inline void StoreU16(std::byte* p, uint16_t v) {
  p[0] = static_cast<std::byte>(v);
  p[1] = static_cast<std::byte>(v >> 8);
}

inline void StoreU32(std::byte* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::byte>(v >> (8 * i));
}

inline void StoreU64(std::byte* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<std::byte>(v >> (8 * i));
}

inline uint16_t LoadU16(const std::byte* p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                               static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8));
}

// The loads are written as one expression of shifted bytes, not as a loop: GCC 11 at -O2 merges
// this expression into a single load on a little-endian CPU (one `movl`, or a `crc32q` reading
// memory directly), but compiles the loop as eight byte loads, variable shifts, and ORs, which made
// the hardware CRC32C slower than the table version on Explorer (S2ab, D103). Clang merges both.
inline uint32_t LoadU32(const std::byte* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
         static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

inline uint64_t LoadU64(const std::byte* p) {
  return static_cast<uint64_t>(p[0]) | static_cast<uint64_t>(p[1]) << 8 |
         static_cast<uint64_t>(p[2]) << 16 | static_cast<uint64_t>(p[3]) << 24 |
         static_cast<uint64_t>(p[4]) << 32 | static_cast<uint64_t>(p[5]) << 40 |
         static_cast<uint64_t>(p[6]) << 48 | static_cast<uint64_t>(p[7]) << 56;
}

// Views text as bytes and bytes as text, without copying.
inline std::span<const std::byte> AsBytes(std::string_view s) {
  return std::as_bytes(std::span<const char>(s.data(), s.size()));
}
inline std::string_view AsChars(std::span<const std::byte> b) {
  return {reinterpret_cast<const char*>(b.data()), b.size()};
}

// Appends little-endian fields to a growing buffer. Encoders validate field lengths before
// writing, so a Str16 longer than 65,535 bytes is a bug (DDS_CHECK), not an input error.
class Writer {
 public:
  void U8(uint8_t v) { buf_.push_back(static_cast<std::byte>(v)); }
  void U16(uint16_t v);
  void U32(uint32_t v);
  void U64(uint64_t v);
  void I64(int64_t v) { U64(static_cast<uint64_t>(v)); }  // two's complement (C++20)
  void Bytes(std::span<const std::byte> data) { buf_.insert(buf_.end(), data.begin(), data.end()); }
  void Str16(std::string_view s);

  size_t size() const { return buf_.size(); }
  const std::vector<std::byte>& buffer() const { return buf_; }
  std::vector<std::byte>& buffer() { return buf_; }
  std::vector<std::byte> Take() { return std::move(buf_); }

 private:
  std::vector<std::byte> buf_;
};

// Reads little-endian fields from untrusted bytes. Every method checks bounds first and returns
// false without advancing or writing its output if the input is too short, so a decoder built on
// Reader cannot read out of bounds whatever bytes it is given (G4). Decoders turn a false into
// Status::Corruption naming offset().
class Reader {
 public:
  explicit Reader(std::span<const std::byte> data) : data_(data) {}

  bool U8(uint8_t* out);
  bool U16(uint16_t* out);
  bool U32(uint32_t* out);
  bool U64(uint64_t* out);
  bool I64(int64_t* out);
  // A view of the next n input bytes (valid as long as the input is).
  bool Bytes(size_t n, std::span<const std::byte>* out);
  // A u16 length followed by that many bytes.
  bool Str16(std::string* out);

  size_t offset() const { return pos_; }
  size_t remaining() const { return data_.size() - pos_; }
  bool empty() const { return pos_ == data_.size(); }

 private:
  std::span<const std::byte> data_;
  size_t pos_ = 0;
};

}  // namespace dds
