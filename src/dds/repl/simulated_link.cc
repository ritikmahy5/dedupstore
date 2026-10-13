#include "dds/repl/simulated_link.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <vector>

#include "dds/common/ordered_mutex.h"
#include "dds/common/thread_annotations.h"

namespace dds {

namespace {

using Clock = std::chrono::steady_clock;

// The state both ends share: one queue per direction, the byte budget, and the closed flag.
class LinkState {
 public:
  explicit LinkState(const LinkOptions& options) : options_(options) {}

  Status Send(size_t direction, std::span<const std::byte> data) DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    if (closed_) return Status::Aborted("simulated link: connection closed");
    size_t n = data.size();
    bool breaks = false;
    if (options_.disconnect_after_bytes.has_value()) {
      const uint64_t budget =
          *options_.disconnect_after_bytes - std::min(sent_, *options_.disconnect_after_bytes);
      if (n > budget) {
        n = static_cast<size_t>(budget);
        breaks = true;
      }
    }
    sent_ += n;
    Direction& d = dirs_[direction];
    if (n > 0) {
      // D28: transmission starts when the link is free, takes size / bandwidth, and the bytes
      // arrive one latency after the last one leaves.
      const Clock::time_point start = std::max(Clock::now(), d.link_free);
      const auto transmit =
          options_.bytes_per_second == 0
              ? Clock::duration(0)
              : std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(
                    static_cast<double>(n) / static_cast<double>(options_.bytes_per_second)));
      d.link_free = start + transmit;
      d.chunks.push_back(
          {std::vector<std::byte>(data.begin(), data.begin() + static_cast<ptrdiff_t>(n)),
           d.link_free + options_.one_way_latency});
    }
    if (breaks) {
      closed_ = true;
      cv_.notify_all();
      return Status::Aborted("simulated link: disconnected");
    }
    cv_.notify_all();
    return Status::Ok();
  }

  Status Receive(size_t direction, std::span<std::byte> out, std::chrono::milliseconds timeout)
      DDS_EXCLUDES(mu_) {
    const Clock::time_point deadline = Clock::now() + timeout;
    MutexLock lock(mu_);
    Direction& d = dirs_[direction];
    size_t filled = 0;
    while (filled < out.size()) {
      const Clock::time_point now = Clock::now();
      if (!d.chunks.empty() && d.chunks.front().arrival <= now) {
        Chunk& c = d.chunks.front();
        const size_t take = std::min(out.size() - filled, c.bytes.size() - c.consumed);
        std::copy_n(c.bytes.begin() + static_cast<ptrdiff_t>(c.consumed), take,
                    out.begin() + static_cast<ptrdiff_t>(filled));
        filled += take;
        c.consumed += take;
        if (c.consumed == c.bytes.size()) d.chunks.pop_front();
        continue;
      }
      // Bytes sent before a break still arrive; after them, a closed link delivers nothing more.
      if (closed_ && d.chunks.empty()) return Status::Aborted("simulated link: disconnected");
      if (now >= deadline) return Status::Aborted("simulated link: receive timed out");
      const Clock::time_point wake =
          d.chunks.empty() ? deadline : std::min(deadline, d.chunks.front().arrival);
      cv_.wait_until(lock, wake);
    }
    return Status::Ok();
  }

  void Close() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    closed_ = true;
    cv_.notify_all();
  }

 private:
  struct Chunk {
    std::vector<std::byte> bytes;
    Clock::time_point arrival;
    size_t consumed = 0;
  };
  struct Direction {
    std::deque<Chunk> chunks;
    Clock::time_point link_free{};
  };

  const LinkOptions options_;
  OrderedMutex mu_{lock_level::kLeaf, "link"};
  std::condition_variable_any cv_;  // waited on with mu_
  std::array<Direction, 2> dirs_ DDS_GUARDED_BY(mu_);
  uint64_t sent_ DDS_GUARDED_BY(mu_) = 0;
  bool closed_ DDS_GUARDED_BY(mu_) = false;
};

class LinkEnd final : public Connection {
 public:
  LinkEnd(std::shared_ptr<LinkState> state, size_t outgoing)
      : state_(std::move(state)), outgoing_(outgoing) {}
  ~LinkEnd() override { state_->Close(); }

  Status Send(std::span<const std::byte> data) override { return state_->Send(outgoing_, data); }
  Status Receive(std::span<std::byte> out, std::chrono::milliseconds timeout) override {
    return state_->Receive(1 - outgoing_, out, timeout);
  }
  void Close() override { state_->Close(); }

 private:
  std::shared_ptr<LinkState> state_;
  const size_t outgoing_;  // the direction this end sends on; it receives on the other
};

}  // namespace

std::pair<std::unique_ptr<Connection>, std::unique_ptr<Connection>> MakeSimulatedLink(
    const LinkOptions& options) {
  auto state = std::make_shared<LinkState>(options);
  return {std::make_unique<LinkEnd>(state, 0), std::make_unique<LinkEnd>(state, 1)};
}

}  // namespace dds
