#pragma once

#include <string>
#include <vector>

namespace dds::testing {

// The result of running the built `dds` binary.
struct DdsRun {
  int exit_code = -1;  // -1 if it did not exit normally
  std::string out;     // stdout
  std::string err;     // stderr
};

// Runs the `dds` binary (path compiled in as DDS_BINARY_PATH) with `args` and waits for it.
// stdout and stderr go to files in a private temporary directory, so neither pipe can fill up
// and block the child.
DdsRun RunDds(const std::vector<std::string>& args);

// A `dds` process left running (for `dds serve`), its stderr connected to a pipe so the test can
// wait for a line without sleeping. The destructor sends SIGTERM and waits.
class DdsProcess {
 public:
  explicit DdsProcess(const std::vector<std::string>& args);
  ~DdsProcess();
  DdsProcess(const DdsProcess&) = delete;
  DdsProcess& operator=(const DdsProcess&) = delete;

  // Reads stderr until a line containing `needle` and returns that line; "" if stderr ended first.
  std::string WaitForLine(const std::string& needle);
  // Sends `signal` and returns the exit code (-1 if the process did not exit normally).
  int Stop(int signal);

 private:
  int pid_ = -1;
  int stderr_fd_ = -1;
  std::string buffered_;
};

}  // namespace dds::testing
