#pragma once

#include <optional>
#include <string>

#include "dds/chunk/chunker.h"
#include "dds/cli/args.h"
#include "dds/cli/command.h"

namespace dds::cli {

// The chunker flags shared by `init` and `chunk-stats`.
inline const std::vector<FlagSpec> kChunkerFlags = {
    {"chunker", true}, {"min-size", true}, {"avg-size", true}, {"max-size", true}};

// Reads --chunker fastcdc|fixed, --min-size, --avg-size and --max-size into `params`, starting
// from its defaults (D9), and validates the result. Returns a usage-error message, or nullopt.
// With --chunker fixed every chunk is --avg-size bytes and --min-size/--max-size are rejected
// (SPEC.md section 3).
std::optional<std::string> ChunkerParamsFromFlags(const ParsedArgs& args, ChunkerParams* params);

}  // namespace dds::cli
