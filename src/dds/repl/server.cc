#include "dds/repl/server.h"

#include <atomic>
#include <thread>

#include "dds/common/log.h"
#include "dds/common/strings.h"
#include "dds/gc/gc_runner.h"
#include "dds/repl/messages.h"

namespace dds {

namespace {

constexpr std::chrono::milliseconds kPoll{200};

}  // namespace

// One accepted connection and the thread serving it.
struct Server::Session {
  std::unique_ptr<TcpConnection> connection;
  std::thread thread;
  std::atomic<bool> done{false};  // set by the thread as its last action; seq_cst
};

Server::Server(Repository& repo, std::unique_ptr<TcpListener> listener, ServerOptions options)
    : repo_(repo), listener_(std::move(listener)), options_(options) {}

Server::~Server() { Reap(/*all=*/true); }

void Server::Reap(bool all) {
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    Session& s = **it;
    if (all) s.connection->Close();  // a blocked receive returns Aborted, and the thread ends
    if (all || s.done) {
      s.thread.join();
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
}

void Server::GcLoop(const std::function<bool()>& stop) {
  GcRunner gc(repo_);
  GcOptions options;
  options.threshold = options_.gc_threshold;
  // Run starts this loop only when an interval is set; the fallback is never used.
  const std::chrono::seconds interval = options_.gc_interval.value_or(std::chrono::hours(1));
  auto next = std::chrono::steady_clock::now() + interval;
  while (!stop()) {
    if (std::chrono::steady_clock::now() < next) {
      // Polling the stop function keeps shutdown within kPoll without another signal channel.
      std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
          kPoll, next - std::chrono::steady_clock::now()));
      continue;
    }
    auto report = gc.Run(options);
    if (report.ok()) {
      LogInfo(StrCat("gc run ", report->run, ": deleted ", report->compact.containers_deleted,
                     ", compacted ", report->compact.containers_compacted, ", reclaimed ",
                     report->bytes_reclaimed, " bytes"));
    } else {
      LogWarn(StrCat("gc failed (the server keeps running): ", report.status().ToString()));
    }
    next = std::chrono::steady_clock::now() + interval;
  }
}

Status Server::Run(const std::function<bool()>& stop) {
  std::thread gc;
  if (options_.gc_interval.has_value()) gc = std::thread([this, &stop] { GcLoop(stop); });
  Status result;
  while (!stop()) {
    Reap(/*all=*/false);
    auto accepted = listener_->Accept(kPoll);
    if (!accepted.ok()) {
      result = accepted.status();
      break;
    }
    if (*accepted == nullptr) continue;
    if (sessions_.size() >= options_.max_connections) {
      (void)WriteFrame(**accepted, MessageType::kError,
                       EncodeError({StatusCode::kBusy, "too many replication connections"}));
      continue;
    }
    auto session = std::make_unique<Session>();
    session->connection = std::move(accepted).value();
    Session* s = session.get();
    s->thread = std::thread([this, s] {
      auto r = ServeReplication(repo_, *s->connection, options_.receiver);
      if (!r.ok()) LogWarn(StrCat("replication session ended: ", r.status().ToString()));
      s->done = true;
    });
    sessions_.push_back(std::move(session));
  }
  Reap(/*all=*/true);
  if (gc.joinable()) gc.join();
  return result;
}

}  // namespace dds
