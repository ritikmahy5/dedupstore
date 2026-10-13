#include "dds/cli/args.h"

#include <string>

namespace dds::cli {

namespace {

const FlagSpec* FindFlag(std::span<const FlagSpec> flags, std::string_view name) {
  for (const FlagSpec& f : flags) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

}  // namespace

std::optional<std::string> ParsedArgs::Get(const std::string& name) const {
  auto it = flags.find(name);
  if (it == flags.end()) return std::nullopt;
  return it->second;
}

std::optional<ParsedArgs> ParseArgs(std::span<const std::string> args,
                                    std::span<const FlagSpec> flags, std::string* error) {
  ParsedArgs out;
  bool flags_done = false;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& word = args[i];
    if (flags_done || word.size() < 3 || !word.starts_with("--")) {
      if (!flags_done && word == "--") {
        flags_done = true;
        continue;
      }
      out.positional.push_back(word);
      continue;
    }
    // "--name" or "--name=value".
    const size_t eq = word.find('=');
    const std::string name = word.substr(2, eq == std::string::npos ? std::string::npos : eq - 2);
    const FlagSpec* spec = FindFlag(flags, name);
    if (spec == nullptr) {
      *error = "unknown option '--" + name + "'";
      return std::nullopt;
    }
    if (out.flags.count(name) > 0) {
      *error = "option '--" + name + "' given more than once";
      return std::nullopt;
    }
    std::string value;
    if (eq != std::string::npos) {
      if (!spec->takes_value) {
        *error = "option '--" + name + "' does not take a value";
        return std::nullopt;
      }
      value = word.substr(eq + 1);
    } else if (spec->takes_value) {
      if (i + 1 >= args.size()) {
        *error = "option '--" + name + "' needs a value";
        return std::nullopt;
      }
      value = args[++i];
    }
    out.flags[name] = std::move(value);
  }
  return out;
}

}  // namespace dds::cli
