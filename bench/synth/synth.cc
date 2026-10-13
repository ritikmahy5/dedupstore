#include "synth.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

#include "dds/chunk/chunk_reader.h"
#include "dds/common/encoding.h"
#include "dds/common/splitmix64.h"
#include "dds/common/strings.h"

namespace dds::bench {

namespace {

// A generator for one (seed, stream) pair. Mixing the stream number through SplitMix64 first makes
// nearby streams (file 1, file 2) independent.
class Rng {
 public:
  Rng(uint64_t seed, uint64_t stream) : state_(seed) { state_ ^= SplitMix64Next(stream); }
  uint64_t Next() { return SplitMix64Next(state_); }
  uint64_t Below(uint64_t n) { return Next() % n; }
  void Fill(std::byte* out, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
      StoreU64(out + i, Next());  // little-endian, so the bytes are the same on every machine
    }
    const uint64_t v = Next();
    for (size_t k = 0; i < n; ++i, ++k) out[i] = static_cast<std::byte>(v >> (8 * k));
  }

 private:
  uint64_t state_;
};

constexpr uint64_t kBaseStream = 0x6261736500000000ULL;  // "base"
constexpr uint64_t kEditStream = 0x6564697400000000ULL;  // "edit"
constexpr uint64_t kSizeStream = 0x73697a6500000000ULL;  // "size"

// Text-like bytes: words of 2 to 9 lowercase letters from a vocabulary of 4096, separated by
// spaces, with a newline every 12 words or so.
void FillText(Rng& rng, std::vector<std::byte>& out, size_t n) {
  static const std::vector<std::string> vocabulary = [] {
    std::vector<std::string> words;
    Rng v(0x766f636162ULL, 0);  // the same vocabulary for every seed
    for (int i = 0; i < 4096; ++i) {
      std::string w(2 + v.Below(8), 'a');
      for (char& c : w) c = static_cast<char>('a' + v.Below(26));
      words.push_back(std::move(w));
    }
    return words;
  }();
  out.reserve(n);
  while (out.size() < n) {
    const std::string& w = vocabulary[rng.Below(vocabulary.size())];
    for (char c : w) out.push_back(static_cast<std::byte>(c));
    out.push_back(static_cast<std::byte>(rng.Below(12) == 0 ? '\n' : ' '));
  }
  out.resize(n);
}

struct FilePlan {
  std::string name;
  bool text;
  uint64_t size;
  uint64_t index;  // stream key
};

std::vector<FilePlan> PlanFiles(const SynthOptions& o) {
  std::vector<FilePlan> plan;
  Rng sizes(o.seed, kSizeStream);
  for (uint64_t kind = 0; kind < 2; ++kind) {
    uint64_t left = kind == 0 ? o.random_bytes : o.text_bytes;
    for (uint64_t i = 0; left > 0; ++i) {
      const uint64_t span = o.max_file - o.min_file + 1;
      const uint64_t size = std::min(left, o.min_file + sizes.Below(span));
      char name[16];
      std::snprintf(name, sizeof(name), "%c%04llu", kind == 0 ? 'r' : 't',
                    static_cast<unsigned long long>(i));
      plan.push_back({name, kind == 1, size, (kind << 32) | i});
      left -= size;
    }
  }
  return plan;
}

// One version's edits to one file: inserts, deletes, and overwrites until ~edit_rate is touched.
void ApplyEdits(Rng& rng, double edit_rate, std::vector<std::byte>& c) {
  const auto target = static_cast<uint64_t>(edit_rate * static_cast<double>(c.size()));
  uint64_t touched = 0;
  std::array<std::byte, 4096> fresh{};
  while (touched < target && !c.empty()) {
    const size_t length = 1 + rng.Below(4096);
    const size_t at = rng.Below(c.size());
    switch (rng.Below(3)) {
      case 0:  // insert
        rng.Fill(fresh.data(), length);
        c.insert(c.begin() + static_cast<ptrdiff_t>(at), fresh.begin(),
                 fresh.begin() + static_cast<ptrdiff_t>(length));
        break;
      case 1: {  // delete
        const size_t n = std::min(length, c.size() - at);
        c.erase(c.begin() + static_cast<ptrdiff_t>(at), c.begin() + static_cast<ptrdiff_t>(at + n));
        break;
      }
      default: {  // overwrite
        const size_t n = std::min(length, c.size() - at);
        rng.Fill(c.data() + at, n);
        break;
      }
    }
    touched += length;
  }
}

}  // namespace

Status GenerateSynth(const SynthOptions& o, uint32_t version,
                     const std::function<Status(SynthFile&&)>& emit) {
  if (o.min_file == 0 || o.min_file > o.max_file || version > o.versions) {
    return Status::InvalidArgument("synth: need 0 < min_file <= max_file and version <= versions");
  }
  for (const FilePlan& f : PlanFiles(o)) {
    SynthFile file;
    file.name = f.name;
    Rng base(o.seed, kBaseStream ^ f.index);
    if (f.text) {
      FillText(base, file.content, f.size);
    } else {
      file.content.resize(f.size);
      base.Fill(file.content.data(), f.size);
    }
    for (uint32_t v = 1; v <= version; ++v) {
      Rng edits(o.seed, kEditStream ^ (uint64_t{v} << 40) ^ f.index);
      ApplyEdits(edits, o.edit_rate, file.content);
    }
    DDS_TRY(emit(std::move(file)));
  }
  return Status::Ok();
}

namespace {

class RandomByteSource final : public ByteSource {
 public:
  RandomByteSource(uint64_t seed, uint64_t file, uint64_t size) : rng_(seed, file), left_(size) {}
  Result<size_t> Read(std::span<std::byte> out) override {
    const size_t n = static_cast<size_t>(std::min<uint64_t>(out.size(), left_));
    rng_.Fill(out.data(), n);
    left_ -= n;
    return n;
  }

 private:
  Rng rng_;
  uint64_t left_;
};

class RandomSourceTree final : public SourceTree {
 public:
  RandomSourceTree(uint64_t seed, uint64_t files, uint64_t size)
      : seed_(seed), files_(files), size_(size) {}

  Result<std::vector<SourceEntry>> ListDir(const std::string& path) override {
    if (!path.empty()) return Status::NotFound(StrCat("no directory ", path));
    std::vector<SourceEntry> out;
    for (uint64_t i = 0; i < files_; ++i) {
      SourceEntry e;
      e.name = Name(i);
      e.kind = SourceKind::kFile;
      e.mode = 0644;
      e.size = size_;
      out.push_back(std::move(e));
    }
    return out;  // names are zero-padded, so this is name order
  }
  Result<std::unique_ptr<ByteSource>> OpenFile(const std::string& path) override {
    if (path.size() != 9 || path[0] != 'f') return Status::NotFound(path);
    const uint64_t index = std::stoull(path.substr(1));
    return std::unique_ptr<ByteSource>(std::make_unique<RandomByteSource>(seed_, index, size_));
  }
  Result<std::string> ReadLink(const std::string& path) override { return Status::NotFound(path); }

 private:
  static std::string Name(uint64_t i) {
    char name[16];
    std::snprintf(name, sizeof(name), "f%08llu", static_cast<unsigned long long>(i));
    return name;
  }
  const uint64_t seed_;
  const uint64_t files_;
  const uint64_t size_;
};

}  // namespace

std::unique_ptr<SourceTree> MakeRandomSourceTree(uint64_t seed, uint64_t files,
                                                 uint64_t file_size) {
  return std::make_unique<RandomSourceTree>(seed, files, file_size);
}

}  // namespace dds::bench
