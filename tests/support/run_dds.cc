#include "support/run_dds.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "support/temp_dir.h"

extern char** environ;

namespace dds::testing {

namespace {

std::string Slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

}  // namespace

DdsRun RunDds(const std::vector<std::string>& args) {
  TempDir io;
  const std::string out_path = io.Sub("stdout");
  const std::string err_path = io.Sub("stderr");

  std::vector<std::string> words = {DDS_BINARY_PATH};
  words.insert(words.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& w : words) argv.push_back(w.data());
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 1, out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                   0644);
  posix_spawn_file_actions_addopen(&actions, 2, err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                   0644);
  pid_t pid = 0;
  DdsRun run;
  const int rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) {
    run.err = "posix_spawn failed for " + words[0];
    return run;
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      run.err = "waitpid failed";
      return run;
    }
  }
  if (WIFEXITED(status)) run.exit_code = WEXITSTATUS(status);
  run.out = Slurp(out_path);
  run.err = Slurp(err_path);
  return run;
}

DdsProcess::DdsProcess(const std::vector<std::string>& args) {
  std::vector<std::string> words = {DDS_BINARY_PATH};
  words.insert(words.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& w : words) argv.push_back(w.data());
  argv.push_back(nullptr);
  int fds[2];
  if (pipe(fds) != 0) return;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
  posix_spawn_file_actions_addclose(&actions, fds[0]);
  posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  if (rc != 0) {
    close(fds[0]);
    return;
  }
  pid_ = pid;
  stderr_fd_ = fds[0];
}

DdsProcess::~DdsProcess() {
  if (pid_ > 0) Stop(SIGTERM);
  if (stderr_fd_ >= 0) close(stderr_fd_);
}

std::string DdsProcess::WaitForLine(const std::string& needle) {
  while (true) {
    size_t newline;
    while ((newline = buffered_.find('\n')) != std::string::npos) {
      std::string line = buffered_.substr(0, newline);
      buffered_.erase(0, newline + 1);
      if (line.find(needle) != std::string::npos) return line;
    }
    char chunk[512];
    const ssize_t n = read(stderr_fd_, chunk, sizeof(chunk));  // blocks until the child writes
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return "";
    buffered_.append(chunk, static_cast<size_t>(n));
  }
}

int DdsProcess::Stop(int signal) {
  if (pid_ <= 0) return -1;
  kill(pid_, signal);
  int status = 0;
  while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
  }
  pid_ = -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace dds::testing
