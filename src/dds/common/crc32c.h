#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace dds {

// CRC32C (Castagnoli): reflected polynomial 0x82F63B78, initial value and final XOR 0xFFFFFFFF,
// so Crc32c("123456789") == 0xE3069283 (ON_DISK_FORMAT.md section 1). It detects random corruption
// of structures and payloads cheaply enough to check on every read; BLAKE3 proves identity (D26).
// Uses the CPU's CRC32C instructions when it has them (SSE4.2 on x86-64, the CRC extension on
// ARMv8), otherwise software slicing-by-8 (D103). Every version computes the same function.
uint32_t Crc32c(std::span<const std::byte> data);

// Continues a CRC over more bytes: Crc32cExtend(Crc32c(a), b) == Crc32c(a ‖ b).
uint32_t Crc32cExtend(uint32_t crc, std::span<const std::byte> data);

// Crc32cExtend with slicing-by-8 even when the CPU has CRC32C instructions, so tests and the
// microbenchmark can compare the two on one machine.
uint32_t Crc32cExtendSoftware(uint32_t crc, std::span<const std::byte> data);

// The same function computed one byte at a time with a single table. Tests compare the other
// versions with it, which must agree on every input.
uint32_t Crc32cBytewise(std::span<const std::byte> data);

// Which version Crc32c uses in this process: "sse4.2", "armv8-crc", or "slicing-by-8". Benchmark
// results record it.
std::string_view Crc32cImplementation();

}  // namespace dds
