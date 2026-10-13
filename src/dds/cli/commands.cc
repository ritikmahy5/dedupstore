#include <vector>

#include "dds/cli/command.h"

namespace dds::cli {

// Each command lives in its own cmd_<name>.cc.
const std::vector<Command>& Commands() {
  static const std::vector<Command> commands = {
      MakeInitCommand(),      MakeBackupCommand(), MakeRestoreCommand(), MakeSnapshotsCommand(),
      MakeLsCommand(),        MakeRmCommand(),     MakeGcCommand(),      MakeServeCommand(),
      MakeReplicateCommand(), MakeVerifyCommand(), MakeStatsCommand(),   MakeChunkStatsCommand(),
  };
  return commands;
}

}  // namespace dds::cli
