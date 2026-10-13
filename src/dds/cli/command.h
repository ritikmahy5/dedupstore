#pragma once

#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "dds/cli/args.h"
#include "dds/common/status.h"

namespace dds::cli {

// Process exit codes (SPEC.md section 3). Library errors map through ExitCodeFor.
inline constexpr int kExitOk = 0;
inline constexpr int kExitUsage = 1;

// kNotFound -> 2, kCorruption -> 3, kBusy -> 4, kIoError -> 5, kAborted -> 6,
// kIncompatible -> 7, kInvalidArgument -> 8, kAlreadyExists -> 9, OK -> 0.
int ExitCodeFor(const Status& status);

// Prints "dds: <status>" to stderr and returns its exit code.
int ReportError(const Status& status);

// Prints "dds: <message>" and the command's usage line to stderr; returns kExitUsage.
int UsageError(const std::string& usage, const std::string& message);

// One subcommand of `dds`.
struct Command {
  std::string name;
  std::string usage;    // "dds <name> <args> [--flags]"
  std::string summary;  // one sentence for `dds --help`
  std::vector<FlagSpec> flags;
  size_t min_positional = 0;
  size_t max_positional = std::numeric_limits<size_t>::max();
  // Runs the parsed command and returns the exit code. Called only when the positional count is
  // within bounds.
  std::function<int(const ParsedArgs&)> run;
};

// Every command, in the order `dds --help` lists them.
const std::vector<Command>& Commands();

// One factory per command, each in cmd_<name>.cc.
Command MakeInitCommand();
Command MakeBackupCommand();
Command MakeRestoreCommand();
Command MakeSnapshotsCommand();
Command MakeLsCommand();
Command MakeRmCommand();
Command MakeGcCommand();
Command MakeServeCommand();
Command MakeReplicateCommand();
Command MakeVerifyCommand();
Command MakeStatsCommand();
Command MakeChunkStatsCommand();

// The whole CLI: parses argv, dispatches, returns the exit code.
int Main(int argc, char** argv);

}  // namespace dds::cli
