#include <cstdio>
#include <string>
#include <vector>

#include "dds/cli/command.h"
#include "dds/common/strings.h"
#include "dds/version.h"

namespace dds::cli {

namespace {

constexpr char kGeneralUsage[] = "dds <command> [arguments] | dds --version | dds --help";

void PrintHelp(std::FILE* out) {
  std::fprintf(out, "usage: %s\n\n", kGeneralUsage);
  std::fprintf(out, "dedupstore %s: a deduplicating, crash-safe snapshot store.\n\n",
               kVersionString);
  std::fprintf(out, "Commands:\n");
  for (const Command& cmd : Commands()) {
    std::fprintf(out, "  %-12s %s\n", cmd.name.c_str(), cmd.summary.c_str());
  }
  std::fprintf(out,
               "\nOptions:\n"
               "  --version    print the version and exit\n"
               "  --help       print this help; `dds <command> --help` describes one command\n"
               "\nResults go to stderr; with --json, commands print one JSON object to stdout.\n");
}

const Command* FindCommand(const std::string& name) {
  for (const Command& cmd : Commands()) {
    if (cmd.name == name) return &cmd;
  }
  return nullptr;
}

int RunCommand(const Command& cmd, const std::vector<std::string>& words) {
  for (const std::string& w : words) {
    if (w == "--") break;
    if (w == "--help") {
      std::printf("usage: %s\n\n%s\n", cmd.usage.c_str(), cmd.summary.c_str());
      return kExitOk;
    }
  }
  std::string error;
  auto parsed = ParseArgs(words, cmd.flags, &error);
  if (!parsed) return UsageError(cmd.usage, error);
  const size_t n = parsed->positional.size();
  if (n < cmd.min_positional) return UsageError(cmd.usage, "missing arguments");
  if (n > cmd.max_positional) return UsageError(cmd.usage, "too many arguments");
  return cmd.run(*parsed);
}

}  // namespace

int ExitCodeFor(const Status& status) {
  switch (status.code()) {
    case StatusCode::kOk:
      return 0;
    case StatusCode::kNotFound:
      return 2;
    case StatusCode::kCorruption:
      return 3;
    case StatusCode::kBusy:
      return 4;
    case StatusCode::kIoError:
      return 5;
    case StatusCode::kAborted:
      return 6;
    case StatusCode::kIncompatible:
      return 7;
    case StatusCode::kInvalidArgument:
      return 8;
    case StatusCode::kAlreadyExists:
      return 9;
  }
  return 5;
}

int ReportError(const Status& status) {
  std::fprintf(stderr, "dds: %s\n", status.ToString().c_str());
  return ExitCodeFor(status);
}

int UsageError(const std::string& usage, const std::string& message) {
  std::fprintf(stderr, "dds: %s\nusage: %s\n", message.c_str(), usage.c_str());
  return kExitUsage;
}

int Main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) return UsageError(kGeneralUsage, "no command given");
  const std::string& first = args[0];
  if (first == "--version") {
    std::printf("dds %s\n", kVersionString);
    return kExitOk;
  }
  if (first == "--help" || first == "help") {
    PrintHelp(stdout);
    return kExitOk;
  }
  if (first.starts_with("--")) {
    return UsageError(kGeneralUsage, StrCat("unknown option '", first, "'"));
  }
  const Command* cmd = FindCommand(first);
  if (cmd == nullptr) return UsageError(kGeneralUsage, StrCat("unknown command '", first, "'"));
  return RunCommand(*cmd, std::vector<std::string>(args.begin() + 1, args.end()));
}

}  // namespace dds::cli
