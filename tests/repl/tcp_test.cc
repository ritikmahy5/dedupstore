// TCP (TESTING.md 3.8): a loopback replication through the real Server, the connection limit, and
// endpoint parsing.

#include "dds/repl/tcp.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "dds/repl/server.h"
#include "repl/repl_fixture.h"
#include "support/test_seed.h"
#include "support/tree_generator.h"

namespace dds {
namespace {

using testing::ReplRepo;

// Runs a Server for `dst` on a free loopback port until destroyed.
class RunningServer {
 public:
  RunningServer(Repository& dst, ServerOptions options = {}) {
    auto listener = TcpListener::Listen({"127.0.0.1", 0});
    EXPECT_TRUE(listener.ok()) << listener.status().ToString();
    port_ = (*listener)->port();
    server_ = std::make_unique<Server>(dst, std::move(listener).value(), options);
    thread_ = std::thread([this] { result_ = server_->Run([this] { return stop_.load(); }); });
  }
  ~RunningServer() {
    stop_ = true;
    thread_.join();
    EXPECT_TRUE(result_.ok()) << result_.ToString();
  }
  uint16_t port() const { return port_; }

 private:
  uint16_t port_ = 0;
  std::unique_ptr<Server> server_;
  std::atomic<bool> stop_{false};
  Status result_;
  std::thread thread_;
};

TEST(TcpReplicationTest, LoopbackReplicationThroughTheServer) {
  ReplRepo src;
  ReplRepo dst;
  testing::TreeGenOptions options;
  options.files = 20;
  options.max_file_size = 256 << 10;
  const MemTree tree = testing::TreeGenerator(testing::TestSeed(), options).NewTree();
  const SnapshotId id = src.Backup(tree);
  {
    RunningServer server(*dst.repo);
    auto connection = TcpConnection::Connect({"127.0.0.1", server.port()});
    ASSERT_TRUE(connection.ok()) << connection.status().ToString();
    auto sent = Replicate(*src.repo, id, **connection, SenderOptions{});
    ASSERT_TRUE(sent.ok()) << sent.status().ToString();
    EXPECT_GT(sent->blobs_sent, 0u);
  }
  EXPECT_TRUE(dst.Restores(id, tree));
  EXPECT_TRUE(dst.Verifies());
}

TEST(TcpReplicationTest, ConnectionsBeyondTheLimitAreRefusedAsBusy) {
  ReplRepo src;
  ReplRepo dst;
  const SnapshotId id =
      src.Backup({{"f", MemNode{EntryKind::kFile, false, 0644, 0, 0, {std::byte{1}}, {}}}});
  ServerOptions options;
  options.max_connections = 1;
  RunningServer server(*dst.repo, options);
  auto idle = TcpConnection::Connect({"127.0.0.1", server.port()});  // holds the only slot
  ASSERT_TRUE(idle.ok());
  // The server registers the first connection before it accepts the next, so this one is refused.
  Status refused;
  for (int attempt = 0; attempt < 50 && refused.code() != StatusCode::kBusy; ++attempt) {
    auto second = TcpConnection::Connect({"127.0.0.1", server.port()});
    ASSERT_TRUE(second.ok());
    refused = Replicate(*src.repo, id, **second, SenderOptions{}).status();
  }
  EXPECT_EQ(refused.code(), StatusCode::kBusy) << refused.ToString();
  (*idle)->Close();
}

TEST(EndpointTest, Parse) {
  auto a = ParseEndpoint("localhost:7000");
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a->host, "localhost");
  EXPECT_EQ(a->port, 7000);
  auto b = ParseEndpoint("[::1]:0");
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(b->host, "::1");
  for (const char* bad : {"", "7000", ":7000", "host:", "host:70000", "host:-1", "host:x"}) {
    EXPECT_EQ(ParseEndpoint(bad).status().code(), StatusCode::kInvalidArgument) << bad;
  }
}

}  // namespace
}  // namespace dds
