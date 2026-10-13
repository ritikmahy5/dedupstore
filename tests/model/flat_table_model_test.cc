// FlatTable versus std::unordered_map (TESTING.md 3.5): one million seeded random
// operations (insert, lookup, erase, relocate) on both. Results must agree after every operation,
// and the full contents are compared periodically.

#include <gtest/gtest.h>

#include <random>
#include <unordered_map>
#include <vector>

#include "dds/index/flat_table.h"
#include "support/test_seed.h"

namespace dds {
namespace {

Fingerprint RandomFingerprint(std::mt19937_64& rng) {
  Fingerprint fp;
  for (size_t i = 0; i < fp.bytes.size(); i += 8) {
    uint64_t v = rng();
    for (size_t j = 0; j < 8; ++j, v >>= 8) fp.bytes[i + j] = static_cast<uint8_t>(v);
  }
  return fp;
}

void CompareContents(const FlatTable& table,
                     const std::unordered_map<Fingerprint, uint32_t, FingerprintHash>& model) {
  ASSERT_EQ(table.size(), model.size());
  size_t seen = 0;
  table.ForEach([&](const IndexEntry& e) {
    ++seen;
    auto it = model.find(e.fp);
    ASSERT_NE(it, model.end());
    ASSERT_EQ(e.container_id, it->second);
  });
  ASSERT_EQ(seen, model.size());
}

TEST(FlatTableModelTest, MillionRandomOperationsAgreeWithUnorderedMap) {
  for (uint64_t seed : testing::TestSeeds(1)) {
    SCOPED_TRACE(testing::SeedMessage(seed));
    std::mt19937_64 rng(seed);
    // A bounded key pool makes inserts of present keys, erases of absent keys, and long-lived
    // probe chains all common. The table grows and shrinks around a few thousand entries.
    std::vector<Fingerprint> pool;
    for (int i = 0; i < 6000; ++i) pool.push_back(RandomFingerprint(rng));
    FlatTable table(16);
    std::unordered_map<Fingerprint, uint32_t, FingerprintHash> model;
    for (int op = 0; op < 1000000; ++op) {
      const Fingerprint& fp = pool[rng() % pool.size()];
      const uint32_t value = static_cast<uint32_t>(rng() % 1000000) + 1;
      switch (rng() % 4) {
        case 0: {  // insert
          IndexEntry e;
          e.fp = fp;
          e.container_id = value;
          e.length = 1;
          const auto [entry, inserted] = table.Insert(e);
          const bool model_inserted = model.emplace(fp, value).second;
          ASSERT_EQ(inserted, model_inserted) << "op " << op;
          ASSERT_EQ(entry->container_id, model.at(fp)) << "op " << op;
          break;
        }
        case 1: {  // lookup
          const IndexEntry* e = table.Find(fp);
          auto it = model.find(fp);
          ASSERT_EQ(e != nullptr, it != model.end()) << "op " << op;
          if (e != nullptr) {
            ASSERT_EQ(e->container_id, it->second) << "op " << op;
          }
          break;
        }
        case 2:  // erase
          ASSERT_EQ(table.Erase(fp), model.erase(fp) == 1) << "op " << op;
          break;
        case 3: {  // relocate: change an existing entry's location in place
          IndexEntry* e = table.Find(fp);
          auto it = model.find(fp);
          ASSERT_EQ(e != nullptr, it != model.end()) << "op " << op;
          if (e != nullptr) {
            e->container_id = value;
            it->second = value;
          }
          break;
        }
      }
      if (op % 50000 == 0) CompareContents(table, model);
    }
    CompareContents(table, model);
  }
}

}  // namespace
}  // namespace dds
