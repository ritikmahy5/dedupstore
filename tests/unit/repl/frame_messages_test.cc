// Frames and messages (PROTOCOL.md sections 2-3): round trips, golden bytes, and rejection of
// everything malformed, since the peer is not trusted (G8).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "dds/common/crc32c.h"
#include "dds/common/encoding.h"
#include "dds/repl/frame.h"
#include "dds/repl/messages.h"
#include "dds/repo/repo_config.h"

namespace dds {
namespace {

Fingerprint Fp(uint8_t fill) {
  Fingerprint fp;
  fp.bytes.fill(fill);
  return fp;
}

TEST(FrameTest, GoldenBytesAndRoundTrip) {
  const std::vector<std::byte> payload = {std::byte{0xAB}, std::byte{0xCD}};
  const auto frame = EncodeFrame(MessageType::kPutAck, payload);
  Writer w;
  w.U32(3);  // type + 2 payload bytes
  w.U8(0x21);
  w.Bytes(payload);
  w.U32(Crc32c(std::span(w.buffer()).subspan(4)));
  EXPECT_EQ(frame, w.buffer());
  auto back = DecodeFrame(frame);
  ASSERT_TRUE(back.ok());
  EXPECT_EQ(back->type, MessageType::kPutAck);
  EXPECT_EQ(back->payload, payload);
}

TEST(FrameTest, EveryBitFlipIsRejected) {
  const auto frame = EncodeFrame(MessageType::kCommit, std::vector<std::byte>(20, std::byte{7}));
  for (size_t byte = 0; byte < frame.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      auto bad = frame;
      bad[byte] ^= static_cast<std::byte>(1 << bit);
      EXPECT_EQ(DecodeFrame(bad).status().code(), StatusCode::kCorruption)
          << "byte " << byte << " bit " << bit;
    }
  }
}

TEST(FrameTest, LengthOutOfRangeAndUnknownTypeAreCorruption) {
  Writer zero;
  zero.U32(0);
  zero.U32(0);
  EXPECT_EQ(DecodeFrame(zero.buffer()).status().code(), StatusCode::kCorruption);
  Writer huge;
  huge.U32(kMaxFrameLength + 1);
  EXPECT_EQ(DecodeFrame(huge.buffer()).status().code(), StatusCode::kCorruption);
  Writer unknown;
  unknown.U32(1);
  unknown.U8(0x55);
  unknown.U32(Crc32c(std::span(unknown.buffer()).subspan(4)));
  EXPECT_EQ(DecodeFrame(unknown.buffer()).status().code(), StatusCode::kCorruption);
}

TEST(MessagesTest, RoundTrips) {
  Hello hello{1, {}, {}, "hash=blake3-256"};
  hello.sender_repo_id.fill(3);
  hello.digest = ChunkingParamsDigest(hello.params_text);
  auto h = DecodeHello(EncodeHello(hello));
  ASSERT_TRUE(h.ok());
  EXPECT_EQ(h->params_text, hello.params_text);
  EXPECT_EQ(h->digest, hello.digest);
  Have have{9, {Fp(1), Fp(2), Fp(3)}};
  auto hv = DecodeHave(EncodeHave(have));
  ASSERT_TRUE(hv.ok());
  EXPECT_EQ(hv->fps, have.fps);
  HaveReply reply{9, {true, false, true, true, false, false, false, false, true}};
  auto hr = DecodeHaveReply(EncodeHaveReply(reply));
  ASSERT_TRUE(hr.ok());
  EXPECT_EQ(hr->present, reply.present);
  Put put{4, {{BlobType::kData, Fp(5), {std::byte{1}}}, {BlobType::kTree, Fp(6), {std::byte{2}}}}};
  auto p = DecodePut(EncodePut(put));
  ASSERT_TRUE(p.ok());
  ASSERT_EQ(p->blobs.size(), 2u);
  EXPECT_EQ(p->blobs[1].type, BlobType::kTree);
  auto e = DecodeError(EncodeError({StatusCode::kIncompatible, "params differ"}));
  ASSERT_TRUE(e.ok());
  EXPECT_EQ(e->code, StatusCode::kIncompatible);
  EXPECT_EQ(StatusFromError(*e).code(), StatusCode::kIncompatible);
}

TEST(MessagesTest, MalformedPayloadsAreCorruption) {
  // HAVE: count 0, count above 4096, and a size that does not match the count.
  Writer have0;
  have0.U32(1);
  have0.U16(0);
  EXPECT_EQ(DecodeHave(have0.buffer()).status().code(), StatusCode::kCorruption);
  Writer have_short;
  have_short.U32(1);
  have_short.U16(2);
  have_short.Bytes(Fp(1).AsBytes());
  EXPECT_EQ(DecodeHave(have_short.buffer()).status().code(), StatusCode::kCorruption);
  // HAVE_REPLY: padding bits set.
  Writer padded;
  padded.U32(1);
  padded.U16(3);
  padded.U8(0x08);  // bit 3 set, but only bits 0-2 are used
  EXPECT_EQ(DecodeHaveReply(padded.buffer()).status().code(), StatusCode::kCorruption);
  // PUT: a blob length past the end of the payload, and an unknown blob type.
  Writer overlong;
  overlong.U32(1);
  overlong.U16(1);
  overlong.U8(1);
  overlong.Bytes(Fp(1).AsBytes());
  overlong.U32(1000);
  overlong.U8(0);
  EXPECT_EQ(DecodePut(overlong.buffer()).status().code(), StatusCode::kCorruption);
  Writer badtype;
  badtype.U32(1);
  badtype.U16(1);
  badtype.U8(9);
  badtype.Bytes(Fp(1).AsBytes());
  badtype.U32(1);
  badtype.U8(0);
  EXPECT_EQ(DecodePut(badtype.buffer()).status().code(), StatusCode::kCorruption);
  // ERROR: code 0 (OK) and an unknown code.
  for (uint16_t code : {uint16_t{0}, uint16_t{99}}) {
    Writer err;
    err.U16(code);
    err.Str16("x");
    EXPECT_EQ(DecodeError(err.buffer()).status().code(), StatusCode::kCorruption) << code;
  }
}

TEST(MessagesTest, ParamsTextIsTheChunkingLinesOfTheConfig) {
  RepoConfig c;
  EXPECT_EQ(ChunkingParamsText(c),
            "hash=blake3-256\nchunker=fastcdc-2016\nmin_size=2048\navg_size=8192\n"
            "max_size=65536\nnormalization=2\ngear_seed=6465647570737472");
}

// The fuzz seed corpora must hold valid inputs (see FuzzCorpusTest in the codec tests).
std::vector<std::vector<std::byte>> Seeds(const std::string& target) {
  std::vector<std::vector<std::byte>> out;
  const auto dir = std::filesystem::path(DDS_FIXTURES_DIR) / "../../fuzz/corpus" / target;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    std::ifstream in(entry.path(), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out.emplace_back(AsBytes(bytes).begin(), AsBytes(bytes).end());
  }
  return out;
}

TEST(FuzzCorpusTest, ReplicationSeedsAreValidInputs) {
  const auto frames = Seeds("frame");
  EXPECT_EQ(frames.size(), 2u);
  for (const auto& seed : frames) EXPECT_TRUE(DecodeFrame(seed).ok());
  const auto messages = Seeds("message");
  EXPECT_EQ(messages.size(), 8u);
  for (const auto& seed : messages) {
    ASSERT_FALSE(seed.empty());
    const auto payload = std::span(seed).subspan(1);
    bool ok = false;
    switch (static_cast<uint8_t>(seed[0]) % 8) {
      case 0:
        ok = DecodeHello(payload).ok();
        break;
      case 1:
        ok = DecodeHelloOk(payload).ok();
        break;
      case 2:
        ok = DecodeHave(payload).ok();
        break;
      case 3:
        ok = DecodeHaveReply(payload).ok();
        break;
      case 4:
        ok = DecodePut(payload).ok();
        break;
      case 5:
        ok = DecodePutAck(payload).ok();
        break;
      case 6:
        ok = DecodeCommitOk(payload).ok();
        break;
      default:
        ok = DecodeError(payload).ok();
        break;
    }
    EXPECT_TRUE(ok) << "selector " << static_cast<int>(seed[0]);
  }
}

}  // namespace
}  // namespace dds
