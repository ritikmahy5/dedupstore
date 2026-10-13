#include "dds/repl/messages.h"

#include <algorithm>

#include "dds/chunk/chunker.h"
#include "dds/common/encoding.h"
#include "dds/common/strings.h"
#include "dds/hash/blake3_hasher.h"
#include "dds/repo/repo_config.h"

namespace dds {

namespace {

Status Malformed(const char* what) { return Status::Corruption(StrCat("malformed ", what)); }

bool ReadArray16(Reader& r, std::array<uint8_t, 16>* out) {
  std::span<const std::byte> b;
  if (!r.Bytes(16, &b)) return false;
  std::transform(b.begin(), b.end(), out->begin(),
                 [](std::byte x) { return static_cast<uint8_t>(x); });
  return true;
}

bool ReadFingerprint(Reader& r, Fingerprint* out) {
  std::span<const std::byte> b;
  if (!r.Bytes(32, &b)) return false;
  std::transform(b.begin(), b.end(), out->bytes.begin(),
                 [](std::byte x) { return static_cast<uint8_t>(x); });
  return true;
}

bool IsBlobType(uint8_t t) { return t >= 1 && t <= 3; }

}  // namespace

std::string ChunkingParamsText(const RepoConfig& c) {
  return StrCat("hash=blake3-256\nchunker=", ChunkerKindName(c.chunker.kind),
                "\nmin_size=", c.chunker.min_size, "\navg_size=", c.chunker.avg_size,
                "\nmax_size=", c.chunker.max_size, "\nnormalization=", c.chunker.normalization,
                "\ngear_seed=", HexU64(c.chunker.gear_seed));
}

ParamsDigest ChunkingParamsDigest(std::string_view params_text) {
  Blake3Hasher h;
  h.Update(AsBytes(params_text));
  const auto full = h.Finalize();
  ParamsDigest d{};
  std::copy(full.begin(), full.begin() + 16, d.begin());
  return d;
}

std::vector<std::byte> EncodeHello(const Hello& m) {
  Writer w;
  w.U16(m.version);
  w.Bytes(std::as_bytes(std::span(m.sender_repo_id)));
  w.Bytes(std::as_bytes(std::span(m.digest)));
  w.Str16(m.params_text);
  return w.Take();
}

Result<Hello> DecodeHello(std::span<const std::byte> p) {
  Reader r(p);
  Hello m;
  if (!r.U16(&m.version) || !ReadArray16(r, &m.sender_repo_id) || !ReadArray16(r, &m.digest) ||
      !r.Str16(&m.params_text) || !r.empty()) {
    return Malformed("HELLO");
  }
  return m;
}

std::vector<std::byte> EncodeHelloOk(const HelloOk& m) {
  Writer w;
  w.U16(m.version);
  w.Bytes(std::as_bytes(std::span(m.receiver_repo_id)));
  return w.Take();
}

Result<HelloOk> DecodeHelloOk(std::span<const std::byte> p) {
  Reader r(p);
  HelloOk m;
  if (!r.U16(&m.version) || !ReadArray16(r, &m.receiver_repo_id) || !r.empty()) {
    return Malformed("HELLO_OK");
  }
  return m;
}

std::vector<std::byte> EncodeHave(const Have& m) {
  Writer w;
  w.U32(m.batch_id);
  w.U16(static_cast<uint16_t>(m.fps.size()));
  for (const Fingerprint& fp : m.fps) w.Bytes(fp.AsBytes());
  return w.Take();
}

Result<Have> DecodeHave(std::span<const std::byte> p) {
  Reader r(p);
  Have m;
  uint16_t count = 0;
  if (!r.U32(&m.batch_id) || !r.U16(&count) || count < 1 || count > kMaxHaveCount ||
      r.remaining() != size_t{count} * 32) {
    return Malformed("HAVE");
  }
  m.fps.resize(count);
  for (Fingerprint& fp : m.fps) ReadFingerprint(r, &fp);
  return m;
}

std::vector<std::byte> EncodeHaveReply(const HaveReply& m) {
  Writer w;
  w.U32(m.batch_id);
  w.U16(static_cast<uint16_t>(m.present.size()));
  std::vector<uint8_t> bitmap((m.present.size() + 7) / 8, 0);
  for (size_t i = 0; i < m.present.size(); ++i) {
    if (m.present[i]) bitmap[i / 8] = static_cast<uint8_t>(bitmap[i / 8] | (1u << (i % 8)));
  }
  w.Bytes(std::as_bytes(std::span(bitmap)));
  return w.Take();
}

Result<HaveReply> DecodeHaveReply(std::span<const std::byte> p) {
  Reader r(p);
  HaveReply m;
  uint16_t count = 0;
  if (!r.U32(&m.batch_id) || !r.U16(&count) || count < 1 || count > kMaxHaveCount ||
      r.remaining() != (size_t{count} + 7) / 8) {
    return Malformed("HAVE_REPLY");
  }
  std::span<const std::byte> bitmap;
  r.Bytes(r.remaining(), &bitmap);
  // Unused high bits of the last byte must be 0 (PROTOCOL.md 3): one encoding per reply.
  if (count % 8 != 0 && (static_cast<uint8_t>(bitmap.back()) >> (count % 8)) != 0) {
    return Malformed("HAVE_REPLY (padding bits set)");
  }
  m.present.resize(count);
  for (size_t i = 0; i < count; ++i) {
    m.present[i] = ((static_cast<uint8_t>(bitmap[i / 8]) >> (i % 8)) & 1) != 0;
  }
  return m;
}

std::vector<std::byte> EncodePut(const Put& m) {
  Writer w;
  w.U32(m.batch_id);
  w.U16(static_cast<uint16_t>(m.blobs.size()));
  for (const PutBlob& b : m.blobs) {
    w.U8(static_cast<uint8_t>(b.type));
    w.Bytes(b.fp.AsBytes());
    w.U32(static_cast<uint32_t>(b.payload.size()));
    w.Bytes(b.payload);
  }
  return w.Take();
}

Result<Put> DecodePut(std::span<const std::byte> p) {
  Reader r(p);
  Put m;
  uint16_t count = 0;
  if (!r.U32(&m.batch_id) || !r.U16(&count) || count < 1) return Malformed("PUT");
  m.blobs.resize(count);
  for (PutBlob& b : m.blobs) {
    uint8_t type = 0;
    uint32_t length = 0;
    std::span<const std::byte> payload;
    if (!r.U8(&type) || !IsBlobType(type) || !ReadFingerprint(r, &b.fp) || !r.U32(&length) ||
        length == 0 || !r.Bytes(length, &payload)) {
      return Malformed("PUT (bad blob header or a length past the end)");
    }
    b.type = static_cast<BlobType>(type);
    b.payload.assign(payload.begin(), payload.end());
  }
  if (!r.empty()) return Malformed("PUT (trailing bytes)");
  return m;
}

std::vector<std::byte> EncodePutAck(uint32_t batch_id) {
  Writer w;
  w.U32(batch_id);
  return w.Take();
}

Result<uint32_t> DecodePutAck(std::span<const std::byte> p) {
  Reader r(p);
  uint32_t id = 0;
  if (!r.U32(&id) || !r.empty()) return Malformed("PUT_ACK");
  return id;
}

std::vector<std::byte> EncodeCommitOk(const std::array<uint8_t, 16>& snapshot_id) {
  Writer w;
  w.Bytes(std::as_bytes(std::span(snapshot_id)));
  return w.Take();
}

Result<std::array<uint8_t, 16>> DecodeCommitOk(std::span<const std::byte> p) {
  Reader r(p);
  std::array<uint8_t, 16> id{};
  if (!ReadArray16(r, &id) || !r.empty()) return Malformed("COMMIT_OK");
  return id;
}

std::vector<std::byte> EncodeError(const ErrorMessage& m) {
  Writer w;
  w.U16(static_cast<uint16_t>(m.code));
  w.Str16(m.message.substr(0, kMaxErrorMessage));
  return w.Take();
}

Result<ErrorMessage> DecodeError(std::span<const std::byte> p) {
  Reader r(p);
  uint16_t code = 0;
  ErrorMessage m;
  if (!r.U16(&code) || !r.Str16(&m.message) || !r.empty() || m.message.size() > kMaxErrorMessage) {
    return Malformed("ERROR");
  }
  // Only the defined non-OK codes (ARCHITECTURE.md section 2's explicit values).
  if (code == 0 || code > static_cast<uint16_t>(StatusCode::kIncompatible)) {
    return Malformed("ERROR (unknown status code)");
  }
  m.code = static_cast<StatusCode>(code);
  return m;
}

Status StatusFromError(const ErrorMessage& m) {
  return Status::FromCode(m.code, StrCat("peer: ", m.message));
}

}  // namespace dds
