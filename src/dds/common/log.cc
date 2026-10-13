#include "dds/common/log.h"

#include <atomic>
#include <cstdio>
#include <string>

namespace dds {

namespace {

// Relaxed is enough: the level is an independent setting. A thread that reads a stale level
// logs or skips one message, and no other data is published through this variable.
std::atomic<int> g_level{static_cast<int>(LogLevel::kWarn)};

char LevelLetter(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
      return 'D';
    case LogLevel::kInfo:
      return 'I';
    case LogLevel::kWarn:
      return 'W';
    case LogLevel::kError:
    case LogLevel::kOff:
      return 'E';
  }
  return '?';
}

}  // namespace

void SetLogLevel(LogLevel level) {
  g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel GetLogLevel() { return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed)); }

void Log(LogLevel level, std::string_view message) {
  if (level == LogLevel::kOff ||
      static_cast<int>(level) < g_level.load(std::memory_order_relaxed)) {
    return;
  }
  std::string line = "dds ";
  line += LevelLetter(level);
  line += ' ';
  line += message;
  line += '\n';
  // One fwrite per line: stdio locks the stream for the call, so lines never interleave.
  (void)std::fwrite(line.data(), 1, line.size(), stderr);
}

}  // namespace dds
