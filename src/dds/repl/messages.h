#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dds/common/result.h"
#include "dds/hash/fingerprint.h"
#include "dds/repl/frame.h"
#include "dds/store/types.h"

namespace dds {

struct RepoConfig;

// PROTOCOL.md section 3. Each Encode produces a payload; each Decode accepts exactly what Encode
// produces and rejects anything else with Corruption (a hostile peer is assumed, G8).
// Version 2 adds HAVE_TREES, for skipping subtrees the receiver already holds whole (D108). A
// receiver accepts versions 1 and 2 and answers with the sender's; a version 1 session never sees
// HAVE_TREES.
inline constexpr uint16_t kProtocolVersion = 2;
inline constexpr uint16_t kOldestProtocolVersion = 1;
inline constexpr size_t kMaxHaveCount = 4096;
inline constexpr size_t kMaxPutCount = 65535;
inline constexpr size_t kMaxErrorMessage = 1024;
using ParamsDigest = std::array<uint8_t, 16>;

// The chunking parameters two repositories must share: the config lines hash through
// normalization, plus gear_seed, joined with '\n'; and the first 16 bytes of its BLAKE3.
std::string ChunkingParamsText(const RepoConfig& config);
ParamsDigest ChunkingParamsDigest(std::string_view params_text);

struct Hello {
  uint16_t version = kProtocolVersion;
  RepoId sender_repo_id{};
  ParamsDigest digest{};
  std::string params_text;
};
struct HelloOk {
  uint16_t version = kProtocolVersion;
  RepoId receiver_repo_id{};
};
struct Have {
  uint32_t batch_id = 0;
  std::vector<Fingerprint> fps;  // 1 to 4096
};
struct HaveReply {
  uint32_t batch_id = 0;
  std::vector<bool> present;  // one per fingerprint of the HAVE
};
// HAVE_TREES and HAVE_TREES_REPLY have the payloads of HAVE and HAVE_REPLY (EncodeHave and
// friends); a reply bit says the tree and every blob below it are present (PROTOCOL.md 3).
struct PutBlob {
  BlobType type = BlobType::kData;
  Fingerprint fp;
  std::vector<std::byte> payload;
};
struct Put {
  uint32_t batch_id = 0;
  std::vector<PutBlob> blobs;  // 1 to 65535
};
struct ErrorMessage {
  StatusCode code = StatusCode::kCorruption;  // never kOk
  std::string message;                        // at most 1024 bytes
};

std::vector<std::byte> EncodeHello(const Hello& m);
Result<Hello> DecodeHello(std::span<const std::byte> p);
std::vector<std::byte> EncodeHelloOk(const HelloOk& m);
Result<HelloOk> DecodeHelloOk(std::span<const std::byte> p);
std::vector<std::byte> EncodeHave(const Have& m);
Result<Have> DecodeHave(std::span<const std::byte> p);
std::vector<std::byte> EncodeHaveReply(const HaveReply& m);
Result<HaveReply> DecodeHaveReply(std::span<const std::byte> p);
// One blob costs a 37-byte header plus its payload (PROTOCOL.md 3).
inline constexpr size_t kPutBlobHeader = 1 + 32 + 4;
std::vector<std::byte> EncodePut(const Put& m);
Result<Put> DecodePut(std::span<const std::byte> p);
std::vector<std::byte> EncodePutAck(uint32_t batch_id);
Result<uint32_t> DecodePutAck(std::span<const std::byte> p);
std::vector<std::byte> EncodeCommitOk(const std::array<uint8_t, 16>& snapshot_id);
Result<std::array<uint8_t, 16>> DecodeCommitOk(std::span<const std::byte> p);
std::vector<std::byte> EncodeError(const ErrorMessage& m);
Result<ErrorMessage> DecodeError(std::span<const std::byte> p);
// The status an ERROR stands for, as the receiving side reports it.
Status StatusFromError(const ErrorMessage& m);

}  // namespace dds
