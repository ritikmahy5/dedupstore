#include "dds/index/flat_table.h"

#include <bit>

#include "dds/common/check.h"

namespace dds {

FlatTable::FlatTable(size_t initial_capacity) : slots_(initial_capacity) {
  // A power of two lets `hash & (capacity - 1)` replace a modulo.
  DDS_CHECK(initial_capacity >= 2 && std::has_single_bit(initial_capacity));
}

size_t FlatTable::Probe(const Fingerprint& fp) const {
  const size_t mask = slots_.size() - 1;
  // Terminates: the load factor stays at most 0.75, so an empty slot always exists.
  for (size_t i = Home(fp);; i = (i + 1) & mask) {
    if (slots_[i].empty() || slots_[i].fp == fp) return i;
  }
}

IndexEntry* FlatTable::Find(const Fingerprint& fp) {
  IndexEntry& slot = slots_[Probe(fp)];
  return slot.empty() ? nullptr : &slot;
}

const IndexEntry* FlatTable::Find(const Fingerprint& fp) const {
  const IndexEntry& slot = slots_[Probe(fp)];
  return slot.empty() ? nullptr : &slot;
}

std::pair<IndexEntry*, bool> FlatTable::Insert(const IndexEntry& entry) {
  DDS_CHECK(!entry.empty());
  // Grow before inserting if the table would pass load factor 3/4.
  if (4 * (size_ + 1) > 3 * slots_.size()) Grow();
  IndexEntry& slot = slots_[Probe(entry.fp)];
  if (!slot.empty()) return {&slot, false};
  slot = entry;
  ++size_;
  return {&slot, true};
}

bool FlatTable::Erase(const Fingerprint& fp) {
  size_t hole = Probe(fp);
  if (slots_[hole].empty()) return false;
  const size_t mask = slots_.size() - 1;
  // Backward-shift deletion (Knuth, TAOCP vol. 3, 6.4, Algorithm R). An entry at slot j got there
  // by probing every slot from its home h up to j, all occupied when it was inserted. After the
  // hole at `hole` opens, a later entry of the cluster may move into it only if the hole lies on
  // its probe path, i.e. if its distance from home, (j - h) mod capacity, is at least its distance
  // from the hole, (j - hole) mod capacity. Otherwise moving it would put it before its home,
  // where lookups never look. Unsigned wraparound plus `& mask` computes the distances mod the
  // power-of-two capacity. The scan stops at the first empty slot, the end of the cluster.
  for (size_t j = (hole + 1) & mask; !slots_[j].empty(); j = (j + 1) & mask) {
    const size_t home = Home(slots_[j].fp);
    if (((j - home) & mask) >= ((j - hole) & mask)) {
      slots_[hole] = slots_[j];
      hole = j;
    }
  }
  slots_[hole] = IndexEntry{};
  --size_;
  return true;
}

void FlatTable::ForEach(const std::function<void(const IndexEntry&)>& fn) const {
  for (const IndexEntry& e : slots_) {
    if (!e.empty()) fn(e);
  }
}

void FlatTable::Reserve(size_t n) {
  size_t capacity = slots_.size();
  while (4 * n > 3 * capacity) capacity *= 2;  // Insert's load-factor rule, for n entries
  if (capacity > slots_.size()) Rehash(capacity);
}

void FlatTable::Grow() { Rehash(slots_.size() * 2); }

void FlatTable::Rehash(size_t capacity) {
  std::vector<IndexEntry> old(capacity);
  old.swap(slots_);
  for (const IndexEntry& e : old) {
    if (!e.empty()) slots_[Probe(e.fp)] = e;
  }
}

}  // namespace dds
