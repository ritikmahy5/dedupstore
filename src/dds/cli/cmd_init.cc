// dds init <repo> [--chunker fastcdc|fixed] [--min-size B] [--avg-size B] [--max-size B]
//                 [--container-size B] [--compression none|zstd[:LEVEL]] [--json]

#include <cstdio>
#include <limits>

#include "dds/cli/chunker_flags.h"
#include "dds/cli/command.h"
#include "dds/cli/json.h"
#include "dds/cli/numbers.h"
#include "dds/common/strings.h"
#include "dds/fs/posix_fs.h"
#include "dds/repo/repository.h"

namespace dds::cli {

namespace {

constexpr char kUsage[] =
    "dds init <repo> [--chunker fastcdc|fixed] [--min-size B] [--avg-size B] [--max-size B] "
    "[--container-size B] [--compression none|zstd[:LEVEL]] [--json]";

void PrintJson(const std::string& path, const RepoConfig& c) {
  JsonWriter w;
  w.BeginObject();
  w.Key("repo");
  w.String(path);
  w.Key("repo_id");
  w.String(HexEncode(std::as_bytes(std::span(c.repo_id))));
  w.Key("chunker");
  w.String(ChunkerKindName(c.chunker.kind));
  w.Key("min_size");
  w.Uint(c.chunker.min_size);
  w.Key("avg_size");
  w.Uint(c.chunker.avg_size);
  w.Key("max_size");
  w.Uint(c.chunker.max_size);
  w.Key("normalization");
  w.Int(c.chunker.normalization);
  w.Key("container_target");
  w.Uint(c.container_target);
  w.Key("compression");
  w.String(CompressionText(c.compression));
  w.EndObject();
  std::printf("%s\n", w.Take().c_str());
}

int Run(const ParsedArgs& args) {
  RepoParams params;
  if (auto error = ChunkerParamsFromFlags(args, &params.chunker)) return UsageError(kUsage, *error);
  if (auto text = args.Get("container-size")) {
    auto size = ParseSize(*text);
    if (!size || *size < kMinContainerTarget || *size > kMaxContainerTarget) {
      return UsageError(kUsage,
                        StrCat("--container-size must be from 1M to 256M, got '", *text, "'"));
    }
    params.container_target = static_cast<uint32_t>(*size);
  }
  if (auto text = args.Get("compression")) {
    auto compression = ParseCompression(*text);
    if (!compression.ok()) return UsageError(kUsage, compression.status().message());
    params.compression = *compression;
  }
  const std::string& path = args.positional[0];
  PosixFs fs;
  auto config = Repository::Init(fs, path, params);
  if (!config.ok()) return ReportError(config.status());
  if (args.Has("json")) {
    PrintJson(path, *config);
  } else {
    std::fprintf(stderr, "initialized repository %s (repo id %s)\n", path.c_str(),
                 HexEncode(std::as_bytes(std::span(config->repo_id))).c_str());
  }
  return kExitOk;
}

}  // namespace

Command MakeInitCommand() {
  Command cmd;
  cmd.name = "init";
  cmd.usage = kUsage;
  cmd.summary = "create an empty repository";
  cmd.flags = kChunkerFlags;
  cmd.flags.push_back({"container-size", true});
  cmd.flags.push_back({"compression", true});
  cmd.flags.push_back({"json", false});
  cmd.min_positional = 1;
  cmd.max_positional = 1;
  cmd.run = Run;
  return cmd;
}

}  // namespace dds::cli
