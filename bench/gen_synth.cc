// dds_gen_synth: writes the `synth` dataset (BENCHMARKS.md section 4, D93) as one directory per
// version: <out>/v0 (the base) through <out>/v<versions>, plus <out>/synth.json recording every
// parameter, so a result can name exactly what it measured.
//
//   dds_gen_synth --out DIR [--seed N] [--random-bytes B] [--text-bytes B] [--versions N]
//                 [--edit-rate F] [--min-file B] [--max-file B]
// Sizes accept K and M suffixes, as dds does.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dds/cli/args.h"
#include "dds/cli/numbers.h"
#include "dds/common/strings.h"
#include "synth/synth.h"

namespace {

int Fail(const std::string& message) {
  std::fprintf(stderr, "dds_gen_synth: %s\n", message.c_str());
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace dds;
  const std::vector<std::string> words(argv + 1, argv + argc);
  const cli::FlagSpec flags[] = {{"out", true},        {"seed", true},     {"random-bytes", true},
                                 {"text-bytes", true}, {"versions", true}, {"edit-rate", true},
                                 {"min-file", true},   {"max-file", true}};
  std::string error;
  auto args = cli::ParseArgs(words, flags, &error);
  if (!args || !args->Get("out")) return Fail(args ? "--out DIR is required" : error);
  bench::SynthOptions o;
  const auto size_flag = [&](const char* name, uint64_t* out) {
    if (auto t = args->Get(name)) {
      auto v = cli::ParseSize(*t);
      if (!v) return false;
      *out = *v;
    }
    return true;
  };
  if (!size_flag("random-bytes", &o.random_bytes) || !size_flag("text-bytes", &o.text_bytes) ||
      !size_flag("min-file", &o.min_file) || !size_flag("max-file", &o.max_file)) {
    return Fail("bad size");
  }
  if (auto t = args->Get("seed")) o.seed = std::strtoull(t->c_str(), nullptr, 10);
  if (auto t = args->Get("versions"))
    o.versions = static_cast<uint32_t>(std::strtoul(t->c_str(), nullptr, 10));
  if (auto t = args->Get("edit-rate")) o.edit_rate = std::strtod(t->c_str(), nullptr);
  const std::filesystem::path out = *args->Get("out");
  for (uint32_t v = 0; v <= o.versions; ++v) {
    const std::filesystem::path dir = out / StrCat("v", v);
    std::filesystem::create_directories(dir);
    const Status written = bench::GenerateSynth(o, v, [&](bench::SynthFile&& f) {
      std::ofstream file(dir / f.name, std::ios::binary | std::ios::trunc);
      file.write(reinterpret_cast<const char*>(f.content.data()),
                 static_cast<std::streamsize>(f.content.size()));
      if (!file) return Status::IoError(StrCat("cannot write ", (dir / f.name).string()));
      return Status::Ok();
    });
    if (!written.ok()) return Fail(written.ToString());
  }
  std::ofstream meta(out / "synth.json");
  meta << "{\"name\":\"synth\",\"seed\":" << o.seed << ",\"random_bytes\":" << o.random_bytes
       << ",\"text_bytes\":" << o.text_bytes << ",\"versions\":" << o.versions
       << ",\"edit_rate\":" << o.edit_rate << ",\"min_file\":" << o.min_file
       << ",\"max_file\":" << o.max_file << "}\n";
  return 0;
}
