#include "dds/common/process_identity.h"

#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>  // kill (POSIX)
#include <fstream>
#include <sstream>
#include <string>

#include "dds/common/strings.h"

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace dds {

std::optional<uint64_t> ProcessStartTime(uint64_t pid) {
#if defined(__APPLE__)
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
  struct kinfo_proc info{};
  size_t len = sizeof(info);
  // For a pid that does not exist, sysctl succeeds and sets len to 0 (checked on macOS 26).
  if (sysctl(mib, 4, &info, &len, nullptr, 0) != 0 || len == 0) return std::nullopt;
  const struct timeval& start = info.kp_proc.p_starttime;
  return static_cast<uint64_t>(start.tv_sec) * 1000000 + static_cast<uint64_t>(start.tv_usec);
#else
  // /proc/<pid>/stat: "pid (comm) state ppid ...". comm may contain spaces and parentheses, so
  // fields are counted from the last ')'. The next field is field 3; starttime is field 22, the
  // 20th field after the ')' (proc(5)).
  std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  if (!std::getline(in, line)) return std::nullopt;
  const size_t close = line.rfind(')');
  if (close == std::string::npos) return std::nullopt;
  std::istringstream fields(line.substr(close + 1));
  std::string field;
  for (int i = 3; i <= 22; ++i) {
    if (!(fields >> field)) return std::nullopt;
  }
  uint64_t start = 0;
  if (!ParseCanonicalU64(field, &start)) return std::nullopt;
  return start;
#endif
}

PosixProcessIdentity::PosixProcessIdentity() : pid_(static_cast<uint64_t>(getpid())) {
  std::array<char, 256> name{};  // POSIX HOST_NAME_MAX is 255 on Linux and macOS
  if (gethostname(name.data(), name.size() - 1) == 0) host_ = name.data();
  if (host_.empty()) host_ = "unknown-host";
  pid_start_ = ProcessStartTime(pid_).value_or(0);
}

bool PosixProcessIdentity::IsAlive(uint64_t pid, uint64_t pid_start) const {
  // kill(pid, 0) checks existence without sending a signal: EPERM means it exists but belongs to
  // someone else, ESRCH that it does not exist (kill(2)).
  if (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) return false;
  const std::optional<uint64_t> start = ProcessStartTime(pid);
  if (!start.has_value()) return false;  // exited between the two checks
  return *start == pid_start;            // a different start time: a reused pid
}

}  // namespace dds
