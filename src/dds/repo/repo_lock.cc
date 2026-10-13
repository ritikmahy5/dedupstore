#include "dds/repo/repo_lock.h"

#include <vector>

#include "dds/common/encoding.h"
#include "dds/common/log.h"
#include "dds/common/strings.h"

namespace dds {

namespace {

constexpr size_t kMaxCommandBytes = 512;
constexpr size_t kMaxHostBytes = 255;
constexpr int kAcquireAttempts = 3;

// Host names go into temporary file names; keep only characters safe in a file name.
std::string FileSafe(std::string_view s) {
  std::string out;
  for (char c : s) {
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '.';
    out += safe ? c : '_';
  }
  return out;
}

Status WriteDurably(Fs& fs, const std::string& path, std::string_view text) {
  auto file = fs.CreateNew(path);
  DDS_TRY(file.status());
  DDS_TRY((*file)->Append(AsBytes(text)));
  DDS_TRY((*file)->Sync());
  return (*file)->Close();
}

std::string Describe(const LockRecord& r) {
  return StrCat("pid ", r.pid, " on host ", r.host, " (command '", r.command, "')");
}

// One attempt at D16's takeover of a lock whose holder is dead. Returns OK if `lock` was moved
// away (the caller then links its own record), Busy if another process took it over first.
Status TakeOverDeadLock(Fs& fs, const RepoLayout& layout, const std::string& stale_text,
                        const std::string& stale_path) {
  Status moved = fs.Rename(layout.lock(), stale_path);
  if (moved.code() == StatusCode::kNotFound) return Status::Ok();  // gone already: just retry
  DDS_TRY(moved);
  auto renamed = ReadWholeFile(fs, stale_path);
  DDS_TRY(renamed.status());
  if (AsChars(*renamed) != stale_text) {
    // Between reading the dead record and renaming, another process took over and installed its
    // own record, and that live lock is what was moved. Put it back with Link, which never
    // replaces a lock that appeared since, and give up.
    (void)fs.Link(stale_path, layout.lock());
    (void)fs.Remove(stale_path);
    return Status::Busy("another process took over the repository's stale lock first");
  }
  (void)fs.Remove(stale_path);
  return Status::Ok();
}

// Links `record_path` to `lock`, or explains why the repository is busy (D16).
Status LinkOrTakeOver(Fs& fs, const RepoLayout& layout, const ProcessIdentity& self,
                      const std::string& record_path, const std::string& tmp_prefix) {
  for (int attempt = 0; attempt < kAcquireAttempts; ++attempt) {
    Status linked = fs.Link(record_path, layout.lock());
    if (linked.code() != StatusCode::kAlreadyExists) return linked;  // OK, or an I/O error
    auto held = ReadWholeFile(fs, layout.lock());
    if (held.status().code() == StatusCode::kNotFound) continue;  // released meanwhile
    DDS_TRY(held.status());
    const std::string held_text(AsChars(*held));
    auto holder = DecodeLockRecord(held_text);
    if (!holder.ok()) {
      return Status::Busy(StrCat(layout.lock(), " exists but does not parse (",
                                 holder.status().message(),
                                 "); remove it if no dds process uses the repository"));
    }
    if (holder->host != self.Host()) {
      return Status::Busy(StrCat("repository is locked by ", Describe(*holder),
                                 "; a lock from another host is never taken over"));
    }
    if (self.IsAlive(holder->pid, holder->pid_start)) {
      return Status::Busy(StrCat("repository is locked by ", Describe(*holder)));
    }
    LogWarn(StrCat("taking over the lock of ", Describe(*holder), ", which is not running"));
    DDS_TRY(TakeOverDeadLock(fs, layout, held_text, StrCat(tmp_prefix, ".stale.", attempt)));
  }
  return Status::Busy("could not acquire the repository lock (it kept changing)");
}

}  // namespace

std::string SanitizeLockCommand(std::string_view command) {
  std::string out(command.substr(0, kMaxCommandBytes));
  for (char& c : out) {
    if (c == '\n' || c == '\r') c = ' ';
  }
  return out;
}

std::string EncodeLockRecord(const LockRecord& r) {
  DDS_CHECK(r.command.size() <= kMaxCommandBytes && r.command.find('\n') == std::string::npos);
  return StrCat("host=", r.host, "\npid=", r.pid, "\npid_start=", r.pid_start,
                "\nacquired_at_ns=", r.acquired_at_ns, "\ncommand=", r.command, "\n");
}

Result<LockRecord> DecodeLockRecord(std::string_view text) {
  static constexpr std::string_view kKeys[] = {"host", "pid", "pid_start", "acquired_at_ns",
                                               "command"};
  std::string_view values[5];
  for (size_t i = 0; i < 5; ++i) {
    const size_t eol = text.find('\n');
    if (eol == std::string_view::npos) return Status::Corruption("lock: truncated record");
    const std::string_view line = text.substr(0, eol);
    text.remove_prefix(eol + 1);
    if (!line.starts_with(kKeys[i]) || line.size() <= kKeys[i].size() ||
        line[kKeys[i].size()] != '=') {
      return Status::Corruption(StrCat("lock: line ", i + 1, ": expected key '", kKeys[i], "'"));
    }
    values[i] = line.substr(kKeys[i].size() + 1);
  }
  if (!text.empty()) return Status::Corruption("lock: unexpected data after the record");
  LockRecord r;
  r.host = values[0];
  r.command = values[4];
  if (r.host.empty() || r.host.size() > kMaxHostBytes) return Status::Corruption("lock: bad host");
  if (r.command.size() > kMaxCommandBytes || r.command.find('\r') != std::string::npos) {
    return Status::Corruption("lock: bad command");
  }
  if (!ParseCanonicalU64(values[1], &r.pid) || !ParseCanonicalU64(values[2], &r.pid_start) ||
      !ParseCanonicalI64(values[3], &r.acquired_at_ns)) {
    return Status::Corruption("lock: bad number");
  }
  return r;
}

Result<std::unique_ptr<RepoLock>> RepoLock::Acquire(Fs& fs, const RepoLayout& layout,
                                                    const ProcessIdentity& self, const Clock& clock,
                                                    std::string_view command) {
  LockRecord record{self.Host(), self.Pid(), self.PidStart(), clock.WallNanos(),
                    SanitizeLockCommand(command)};
  if (record.host.size() > kMaxHostBytes) record.host.resize(kMaxHostBytes);
  const std::string text = EncodeLockRecord(record);
  // ON_DISK_FORMAT.md section 4: tmp/lock.<host>.<pid>.<nanos>, written and synced before the
  // link, so `lock` never exists without its full contents.
  const std::string tmp_prefix =
      JoinPath(layout.tmp(),
               StrCat("lock.", FileSafe(record.host), ".", record.pid, ".", record.acquired_at_ns));
  DDS_TRY(WriteDurably(fs, tmp_prefix, text));
  Status status = LinkOrTakeOver(fs, layout, self, tmp_prefix, tmp_prefix);
  (void)fs.Remove(tmp_prefix);  // `lock`, if linked, is a second name for the same file
  DDS_TRY(status);
  return std::make_unique<RepoLock>(Key(), fs, layout, std::move(record), text);
}

RepoLock::~RepoLock() {
  if (held_) (void)Release();
}

Status RepoLock::Release() {
  if (!held_) return Status::Ok();
  held_ = false;
  auto current = ReadWholeFile(fs_, layout_.lock());
  if (!current.ok()) return current.status().WithContext("releasing the repository lock");
  if (AsChars(*current) != text_) {
    // Not our record any more: never remove another process's lock.
    LogWarn(StrCat(layout_.lock(), " no longer holds this process's record; leaving it"));
    return Status::Ok();
  }
  return fs_.Remove(layout_.lock());
}

}  // namespace dds
