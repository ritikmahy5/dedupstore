// BoundedQueue: FIFO order, the capacity bound, Close semantics, and many producers and
// consumers losing and duplicating nothing.

#include "dds/common/bounded_queue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace dds {
namespace {

TEST(BoundedQueueTest, FifoAndCloseDrainsWhatIsLeft) {
  BoundedQueue<int> q(4);
  EXPECT_TRUE(q.Push(1));
  EXPECT_TRUE(q.Push(2));
  q.Close();
  EXPECT_FALSE(q.Push(3));
  EXPECT_EQ(q.Pop(), 1);
  EXPECT_EQ(q.Pop(), 2);
  EXPECT_EQ(q.Pop(), std::nullopt);
}

TEST(BoundedQueueTest, PushBlocksWhileFull) {
  BoundedQueue<int> q(2);
  ASSERT_TRUE(q.Push(1));
  ASSERT_TRUE(q.Push(2));
  std::atomic<bool> pushed{false};
  std::thread producer([&] {
    EXPECT_TRUE(q.Push(3));  // blocks until a Pop makes room
    pushed = true;
  });
  // While the queue holds two items the producer cannot finish, so `pushed` must still be false
  // here. One-sided: correct code never fails this check; a Push that did not wait fails it
  // whenever the producer runs first. (No sleep: the join below orders the rest.)
  EXPECT_FALSE(pushed);
  EXPECT_EQ(q.Pop(), 1);
  producer.join();
  EXPECT_TRUE(pushed);
  EXPECT_EQ(q.Pop(), 2);
  EXPECT_EQ(q.Pop(), 3);
}

TEST(BoundedQueueTest, CloseWakesABlockedProducerAndConsumer) {
  BoundedQueue<int> full(1);
  ASSERT_TRUE(full.Push(1));
  std::thread producer([&] { EXPECT_FALSE(full.Push(2)); });
  BoundedQueue<int> empty(1);
  std::thread consumer([&] { EXPECT_EQ(empty.Pop(), std::nullopt); });
  full.Close();
  empty.Close();
  producer.join();
  consumer.join();
}

TEST(BoundedQueueTest, ManyProducersAndConsumersDeliverEveryItemOnce) {
  constexpr int kProducers = 4, kConsumers = 4, kPerProducer = 5000;
  BoundedQueue<int> q(8);
  std::vector<std::atomic<int>> seen(kProducers * kPerProducer);
  std::vector<std::thread> threads;
  for (int c = 0; c < kConsumers; ++c) {
    threads.emplace_back([&] {
      while (auto item = q.Pop()) seen[static_cast<size_t>(*item)].fetch_add(1);
    });
  }
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 0; i < kPerProducer; ++i) ASSERT_TRUE(q.Push(p * kPerProducer + i));
    });
  }
  for (auto& t : producers) t.join();
  q.Close();
  for (auto& t : threads) t.join();
  for (size_t i = 0; i < seen.size(); ++i) ASSERT_EQ(seen[i].load(), 1) << "item " << i;
}

}  // namespace
}  // namespace dds
