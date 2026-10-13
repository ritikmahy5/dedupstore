// BLAKE3 against the official test vectors: test_vectors/test_vectors.json from the
// BLAKE3 release this build fetched (D58). Each case gives an input length; the input is the
// repeating byte sequence 0, 1, ..., 250, and "hash" is the 131-byte extended output, whose first
// 32 bytes are the default-length hash.

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "dds/common/strings.h"
#include "dds/hash/blake3_hasher.h"

namespace dds {
namespace {

struct Blake3Vector {
  size_t input_len = 0;
  std::string hash_hex;  // 131 bytes of extended output, as hex
};

// The file is regular JSON written by the BLAKE3 authors; pulling out the two fields needed here
// with string search avoids writing a JSON parser for a test (D4 allows no JSON library).
std::vector<Blake3Vector> LoadVectors() {
  std::ifstream in(DDS_BLAKE3_TEST_VECTORS);
  std::stringstream text;
  text << in.rdbuf();
  const std::string json = text.str();
  std::vector<Blake3Vector> out;
  size_t pos = 0;
  while ((pos = json.find("\"input_len\":", pos)) != std::string::npos) {
    Blake3Vector v;
    pos += 12;
    v.input_len = std::stoul(json.substr(pos, 20));
    const size_t hash_key = json.find("\"hash\":", pos);
    const size_t open = json.find('"', hash_key + 7);
    const size_t close = json.find('"', open + 1);
    v.hash_hex = json.substr(open + 1, close - open - 1);
    out.push_back(v);
    pos = close;
  }
  return out;
}

std::vector<std::byte> PatternInput(size_t len) {
  std::vector<std::byte> input(len);
  for (size_t i = 0; i < len; ++i) input[i] = static_cast<std::byte>(i % 251);
  return input;
}

TEST(Blake3Test, OfficialTestVectors) {
  const std::vector<Blake3Vector> vectors = LoadVectors();
  ASSERT_GE(vectors.size(), 30u) << "could not read " << DDS_BLAKE3_TEST_VECTORS;
  for (const Blake3Vector& v : vectors) {
    SCOPED_TRACE("input_len=" + std::to_string(v.input_len));
    const std::vector<std::byte> input = PatternInput(v.input_len);
    Blake3Hasher hasher;
    hasher.Update(input);
    std::vector<std::byte> extended(v.hash_hex.size() / 2);
    hasher.FinalizeExtended(extended);
    EXPECT_EQ(HexEncode(extended), v.hash_hex);
    const auto digest = hasher.Finalize();
    EXPECT_EQ(HexEncode(std::as_bytes(std::span(digest))), v.hash_hex.substr(0, 64));
  }
}

TEST(Blake3Test, IncrementalUpdatesMatchOneShot) {
  const std::vector<std::byte> input = PatternInput(100000);
  Blake3Hasher one;
  one.Update(input);
  for (size_t piece :
       {size_t{1}, size_t{63}, size_t{64}, size_t{1023}, size_t{1024}, size_t{4097}}) {
    Blake3Hasher many;
    for (size_t off = 0; off < input.size(); off += piece) {
      many.Update(std::span(input).subspan(off, std::min(piece, input.size() - off)));
    }
    EXPECT_EQ(many.Finalize(), one.Finalize()) << "piece=" << piece;
  }
}

TEST(Blake3Test, FinalizeDoesNotResetState) {
  Blake3Hasher h;
  h.Update(PatternInput(10));
  const auto first = h.Finalize();
  EXPECT_EQ(h.Finalize(), first);
}

}  // namespace
}  // namespace dds
