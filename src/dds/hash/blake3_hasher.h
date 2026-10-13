#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "blake3.h"

namespace dds {

// Incremental BLAKE3 over the official C implementation (D4, D7). Feed bytes with Update, read the
// 32-byte hash with Finalize. Finalize does not reset the state, so it may be called again (or
// more input added) afterwards. Not thread-safe; one hasher per thread.
class Blake3Hasher {
 public:
  Blake3Hasher() { blake3_hasher_init(&state_); }

  void Update(std::span<const std::byte> data) {
    blake3_hasher_update(&state_, data.data(), data.size());
  }

  std::array<uint8_t, 32> Finalize() const {
    std::array<uint8_t, 32> out{};
    blake3_hasher_finalize(&state_, out.data(), out.size());
    return out;
  }

  // Extended output of any length (BLAKE3 is an XOF); the first 32 bytes equal Finalize().
  // Used by the official test vectors, which are 131 bytes long.
  void FinalizeExtended(std::span<std::byte> out) const {
    blake3_hasher_finalize(&state_, reinterpret_cast<uint8_t*>(out.data()), out.size());
  }

 private:
  blake3_hasher state_;
};

}  // namespace dds
