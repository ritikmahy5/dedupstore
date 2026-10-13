#pragma once

#include <string_view>

namespace dds {

// Leveled logging to stderr. Library code logs warnings it cannot return (a skipped special
// file, a background GC failure); errors are returned as Status. Each call writes one complete
// line with a single fwrite, and stdio locks the stream for the duration of each call, so lines
// from different threads never interleave.
enum class LogLevel { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3, kOff = 4 };

void SetLogLevel(LogLevel level);
LogLevel GetLogLevel();

// Writes "dds <L> <message>\n" (L is D, I, W, or E) if `level` is at or above the current level.
void Log(LogLevel level, std::string_view message);
inline void LogDebug(std::string_view message) { Log(LogLevel::kDebug, message); }
inline void LogInfo(std::string_view message) { Log(LogLevel::kInfo, message); }
inline void LogWarn(std::string_view message) { Log(LogLevel::kWarn, message); }
inline void LogError(std::string_view message) { Log(LogLevel::kError, message); }

}  // namespace dds
