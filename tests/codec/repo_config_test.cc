// `config` codec (ON_DISK_FORMAT.md section 3; TESTING.md 3.3).

#include "dds/repo/repo_config.h"

#include <gtest/gtest.h>

#include <random>
#include <string>

#include "dds/common/crc32c.h"
#include "dds/common/encoding.h"
#include "dds/common/strings.h"
#include "support/test_seed.h"

namespace dds {
namespace {

RepoConfig DefaultConfig() {
  RepoConfig c;
  for (size_t i = 0; i < c.repo_id.size(); ++i) c.repo_id[i] = static_cast<uint8_t>(i * 17);
  c.created_at_ns = 1791331200000000000;
  return c;  // default chunker parameters and a 4 MiB container target
}

// Appends the crc32c line over `body`, as the format requires.
std::string WithCrc(const std::string& body) {
  return body + "crc32c=" + HexU32(Crc32c(AsBytes(body))) + "\n";
}

const char kGoldenBody[] =
    "dedupstore-config=2\n"
    "repo_id=00112233445566778899aabbccddeeff\n"
    "created_at_ns=1791331200000000000\n"
    "hash=blake3-256\n"
    "chunker=fastcdc-2016\n"
    "min_size=2048\n"
    "avg_size=8192\n"
    "max_size=65536\n"
    "normalization=2\n"
    "gear_seed=6465647570737472\n"
    "container_target=4194304\n"
    "compression=none\n";

// Version 1, before D107: no compression line.
const char kGoldenBodyV1[] =
    "dedupstore-config=1\n"
    "repo_id=00112233445566778899aabbccddeeff\n"
    "created_at_ns=1791331200000000000\n"
    "hash=blake3-256\n"
    "chunker=fastcdc-2016\n"
    "min_size=2048\n"
    "avg_size=8192\n"
    "max_size=65536\n"
    "normalization=2\n"
    "gear_seed=6465647570737472\n"
    "container_target=4194304\n";

TEST(RepoConfigTest, GoldenBytes) {
  EXPECT_EQ(EncodeRepoConfig(DefaultConfig()), WithCrc(kGoldenBody));
  auto decoded = DecodeRepoConfig(WithCrc(kGoldenBody));
  ASSERT_TRUE(decoded.ok()) << decoded.status().ToString();
  EXPECT_EQ(decoded->repo_id, DefaultConfig().repo_id);
  EXPECT_EQ(decoded->chunker.avg_size, 8192u);
  EXPECT_EQ(decoded->container_target, 4194304u);
  EXPECT_EQ(decoded->compression, CompressionParams{});
}

// Repositories made before D107 have version 1 configs: they decode, with compression none, and
// encode back to the same bytes.
TEST(RepoConfigTest, Version1StillDecodesAndRoundTrips) {
  auto decoded = DecodeRepoConfig(WithCrc(kGoldenBodyV1));
  ASSERT_TRUE(decoded.ok()) << decoded.status().ToString();
  EXPECT_EQ(decoded->format_version, 1u);
  EXPECT_EQ(decoded->compression, CompressionParams{});
  EXPECT_EQ(EncodeRepoConfig(*decoded), WithCrc(kGoldenBodyV1));
  RepoConfig v1_with_zstd = *decoded;
  v1_with_zstd.compression = {Codec::kZstd, 3};
  EXPECT_FALSE(ValidateRepoConfig(v1_with_zstd).ok());  // version 1 cannot record it
}

TEST(RepoConfigTest, CompressionLine) {
  auto with = [](const std::string& value) {
    std::string body = kGoldenBody;
    body.replace(body.find("compression=none"), 16, "compression=" + value);
    return WithCrc(body);
  };
  auto zstd = DecodeRepoConfig(with("zstd:7"));
  ASSERT_TRUE(zstd.ok()) << zstd.status().ToString();
  EXPECT_EQ(zstd->compression, (CompressionParams{Codec::kZstd, 7}));
  EXPECT_EQ(EncodeRepoConfig(*zstd), with("zstd:7"));
  for (const char* bad : {"zstd", "zstd:0", "zstd:20", "zstd:07", "lz4", "zstd:3 ", ""}) {
    EXPECT_EQ(DecodeRepoConfig(with(bad)).status().code(), StatusCode::kCorruption) << bad;
  }
  EXPECT_EQ(ParseCompression("zstd")->level, 3);  // the CLI's shorthand, never written to config
  EXPECT_FALSE(ParseCompression("zstd:20").ok());
}

TEST(RepoConfigTest, RoundTripOverRandomValidConfigs) {
  for (uint64_t seed : testing::TestSeeds(5)) {
    SCOPED_TRACE(testing::SeedMessage(seed));
    std::mt19937_64 rng(seed);
    for (int i = 0; i < 200; ++i) {
      RepoConfig c;
      for (auto& b : c.repo_id) b = static_cast<uint8_t>(rng());
      c.created_at_ns = static_cast<int64_t>(rng());  // any i64, negative included
      c.chunker.avg_size = 1u << (8 + rng() % 15);    // 256 .. 4 MiB
      if (rng() % 4 == 0) {
        c.chunker.kind = ChunkerKind::kFixed;
        c.chunker.min_size = c.chunker.max_size = c.chunker.avg_size;
        c.chunker.normalization = 0;
      } else {
        c.chunker.min_size = 64 + static_cast<uint32_t>(rng() % (c.chunker.avg_size - 64));
        c.chunker.max_size =
            c.chunker.avg_size + 1 + static_cast<uint32_t>(rng() % (16777216 - c.chunker.avg_size));
        c.chunker.normalization = static_cast<int>(rng() % 4);
      }
      c.chunker.gear_seed = rng();
      c.container_target = (1u << 20) + static_cast<uint32_t>(rng() % ((255u << 20) + 1));
      if (rng() % 2 == 0) c.compression = {Codec::kZstd, 1 + static_cast<int>(rng() % 19)};
      ASSERT_TRUE(ValidateRepoConfig(c).ok());
      const std::string text = EncodeRepoConfig(c);
      auto back = DecodeRepoConfig(text);
      ASSERT_TRUE(back.ok()) << back.status().ToString() << "\n" << text;
      EXPECT_EQ(EncodeRepoConfig(*back), text);  // canonical: decode + encode reproduces the bytes
    }
  }
}

TEST(RepoConfigTest, EverySingleBitFlipIsRejected) {
  const std::string good = WithCrc(kGoldenBody);
  for (size_t byte = 0; byte < good.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string flipped = good;
      flipped[byte] = static_cast<char>(flipped[byte] ^ (1 << bit));
      auto decoded = DecodeRepoConfig(flipped);
      ASSERT_FALSE(decoded.ok()) << "byte " << byte << " bit " << bit;
      const StatusCode code = decoded.status().code();
      EXPECT_TRUE(code == StatusCode::kCorruption || code == StatusCode::kIncompatible)
          << decoded.status().ToString();
    }
  }
}

TEST(RepoConfigTest, UnknownVersionIsIncompatible) {
  std::string body = kGoldenBody;
  body.replace(body.find("config=2"), 8, "config=3");
  EXPECT_EQ(DecodeRepoConfig(WithCrc(body)).status().code(), StatusCode::kIncompatible);
}

TEST(RepoConfigTest, NonCanonicalFormsAreCorruption) {
  auto with = [](std::string from, std::string to) {
    std::string body = kGoldenBody;
    body.replace(body.find(from), from.size(), to);
    return WithCrc(body);
  };
  const std::string cases[] = {
      with("min_size=2048", "min_size=02048"),  // leading zero
      with("repo_id=00112233445566778899aabbccddeeff",
           "repo_id=00112233445566778899AABBCCDDEEFF"),  // uppercase hex
      with("hash=blake3-256", "hash=sha256"),            // unknown hash
      with("chunker=fastcdc-2016", "chunker=rabin"),
      with("normalization=2", "normalization=4"),                            // out of range
      with("avg_size=8192", "avg_size=8000"),                                // not a power of two
      with("container_target=4194304", "container_target=1000"),             // below 1 MiB
      with("min_size=2048\navg_size=8192", "avg_size=8192\nmin_size=2048"),  // key order
      with("hash=blake3-256", "hash =blake3-256"),                           // whitespace
      with("normalization=2\n", "normalization=2\nextra=1\n"),               // unknown key
      with("normalization=2\n", ""),                                         // missing key
      WithCrc(std::string(kGoldenBody) + "\n"),                              // blank line
      std::string(kGoldenBody),                                              // no crc line
      WithCrc(kGoldenBody).substr(0, WithCrc(kGoldenBody).size() - 1),       // no final newline
  };
  for (const std::string& text : cases) {
    EXPECT_EQ(DecodeRepoConfig(text).status().code(), StatusCode::kCorruption) << text;
  }
}

TEST(RepoConfigTest, FixedChunkerRules) {
  RepoConfig c = DefaultConfig();
  c.chunker.kind = ChunkerKind::kFixed;
  c.chunker.min_size = c.chunker.max_size = c.chunker.avg_size = 8192;
  c.chunker.normalization = 0;
  EXPECT_TRUE(ValidateRepoConfig(c).ok());
  auto back = DecodeRepoConfig(EncodeRepoConfig(c));
  ASSERT_TRUE(back.ok());
  EXPECT_EQ(back->chunker.kind, ChunkerKind::kFixed);
  c.chunker.min_size = 2048;
  EXPECT_EQ(ValidateRepoConfig(c).code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace dds
