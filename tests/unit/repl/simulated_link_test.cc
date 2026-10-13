// SimulatedLink (D28): delivery in order, the byte-exact disconnect, closing, and timeouts.

#include "dds/repl/simulated_link.h"

#include <gtest/gtest.h>

#include <thread>

namespace dds {
namespace {

using std::chrono::milliseconds;

std::vector<std::byte> Bytes(size_t n, uint8_t fill) {
  return std::vector<std::byte>(n, std::byte{fill});
}

TEST(SimulatedLinkTest, DeliversBothDirectionsInOrder) {
  auto [a, b] = MakeSimulatedLink({});
  ASSERT_TRUE(a->Send(Bytes(3, 1)).ok());
  ASSERT_TRUE(a->Send(Bytes(2, 2)).ok());
  ASSERT_TRUE(b->Send(Bytes(1, 9)).ok());
  std::vector<std::byte> got(5);
  ASSERT_TRUE(b->Receive(got, milliseconds(1000)).ok());
  EXPECT_EQ(got, (std::vector<std::byte>{std::byte{1}, std::byte{1}, std::byte{1}, std::byte{2},
                                         std::byte{2}}));
  std::vector<std::byte> back(1);
  ASSERT_TRUE(a->Receive(back, milliseconds(1000)).ok());
  EXPECT_EQ(back[0], std::byte{9});
}

TEST(SimulatedLinkTest, DisconnectDeliversExactlyTheBytesBeforeTheOffset) {
  LinkOptions options;
  options.disconnect_after_bytes = 5;
  auto [a, b] = MakeSimulatedLink(options);
  ASSERT_TRUE(a->Send(Bytes(3, 1)).ok());
  EXPECT_EQ(a->Send(Bytes(4, 2)).code(), StatusCode::kAborted);  // only 2 of these 4 go out
  std::vector<std::byte> got(5);
  ASSERT_TRUE(b->Receive(got, milliseconds(1000)).ok());
  EXPECT_EQ(got[4], std::byte{2});
  std::vector<std::byte> more(1);
  EXPECT_EQ(b->Receive(more, milliseconds(1000)).code(), StatusCode::kAborted);
  EXPECT_EQ(b->Send(Bytes(1, 0)).code(), StatusCode::kAborted);  // both directions are down
}

TEST(SimulatedLinkTest, CloseWakesABlockedReceiver) {
  auto [a, b] = MakeSimulatedLink({});
  Status received;
  std::thread reader([&, end = b.get()] {
    std::vector<std::byte> buf(1);
    received = end->Receive(buf, milliseconds(60000));
  });
  a->Close();
  reader.join();
  EXPECT_EQ(received.code(), StatusCode::kAborted);
}

TEST(SimulatedLinkTest, ReceiveTimesOut) {
  auto [a, b] = MakeSimulatedLink({});
  std::vector<std::byte> buf(1);
  EXPECT_EQ(b->Receive(buf, milliseconds(20)).code(), StatusCode::kAborted);
}

TEST(SimulatedLinkTest, LatencyDelaysDelivery) {
  LinkOptions options;
  options.one_way_latency = std::chrono::milliseconds(50);
  auto [a, b] = MakeSimulatedLink(options);
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(a->Send(Bytes(1, 1)).ok());
  std::vector<std::byte> buf(1);
  ASSERT_TRUE(b->Receive(buf, milliseconds(5000)).ok());
  EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(50));
}

}  // namespace
}  // namespace dds
