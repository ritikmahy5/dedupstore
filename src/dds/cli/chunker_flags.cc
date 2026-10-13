#include "dds/cli/chunker_flags.h"

#include <cstdint>
#include <limits>

#include "dds/cli/numbers.h"
#include "dds/common/strings.h"

namespace dds::cli {

std::optional<std::string> ChunkerParamsFromFlags(const ParsedArgs& args, ChunkerParams* params) {
  const std::string kind = args.Get("chunker").value_or("fastcdc");
  if (kind == "fixed") {
    if (args.Has("min-size") || args.Has("max-size")) {
      return "--min-size and --max-size do not apply to --chunker fixed";
    }
    params->kind = ChunkerKind::kFixed;
    params->normalization = 0;
  } else if (kind != "fastcdc") {
    return "unknown chunker '" + kind + "' (expected fastcdc or fixed)";
  }
  struct SizeFlag {
    const char* name;
    uint32_t* target;
  };
  for (const SizeFlag& flag :
       {SizeFlag{"min-size", &params->min_size}, SizeFlag{"avg-size", &params->avg_size},
        SizeFlag{"max-size", &params->max_size}}) {
    auto text = args.Get(flag.name);
    if (!text) continue;
    auto size = ParseSize(*text);
    if (!size || *size > std::numeric_limits<uint32_t>::max()) {
      return StrCat("bad size for --", flag.name, ": '", *text, "'");
    }
    *flag.target = static_cast<uint32_t>(*size);
  }
  if (params->kind == ChunkerKind::kFixed) params->min_size = params->max_size = params->avg_size;
  if (Status valid = ValidateChunkerParams(*params); !valid.ok()) return valid.message();
  return std::nullopt;
}

}  // namespace dds::cli
