#pragma once

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dds::cli {

// One flag a command accepts: `--name` alone (a switch) or `--name VALUE` / `--name=VALUE`.
struct FlagSpec {
  std::string name;  // without the leading "--"
  bool takes_value = false;
};

// A command line after parsing: positional arguments in order and the flags that were given.
struct ParsedArgs {
  std::vector<std::string> positional;
  std::map<std::string, std::string> flags;  // a switch maps to ""

  bool Has(const std::string& name) const { return flags.count(name) > 0; }
  std::optional<std::string> Get(const std::string& name) const;
};

// Parses `args` (the words after the command name) against `flags`. Returns nullopt and sets
// `error` for a usage error: an unknown flag, a flag given twice, a missing value, or a value
// given to a switch. A lone "--" ends flag parsing; later words are positional even if they start
// with "--". The CLI reports any parse failure as exit code 1 (SPEC.md section 3).
std::optional<ParsedArgs> ParseArgs(std::span<const std::string> args,
                                    std::span<const FlagSpec> flags, std::string* error);

}  // namespace dds::cli
