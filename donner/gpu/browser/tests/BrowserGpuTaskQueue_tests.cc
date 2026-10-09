#include "donner/gpu/browser/BrowserGpuTaskQueue.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace donner::gpu::browser {
namespace {

TEST(BrowserGpuTaskQueueTest, PreservesFifoAndEmptyLatePulses) {
  BrowserGpuTaskQueue queue;
  std::vector<int> calls;
  struct Context {
    std::vector<int>& calls;
    int value;
  } first{calls, 1}, second{calls, 2};
  const auto record = [](void* data) {
    auto& context = *static_cast<Context*>(data);
    context.calls.push_back(context.value);
  };
  BrowserGpuTask a{record, &first}, b{record, &second};
  queue.enqueue(a);
  queue.enqueue(b);
  EXPECT_EQ(queue.executeBatch(), 2u);
  EXPECT_EQ(calls, (std::vector<int>{1, 2}));
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.executeBatch(), 0u);
}

TEST(BrowserGpuTaskQueueTest, NewRequestsCannotExtendExecutingBatch) {
  BrowserGpuTaskQueue queue;
  int calls = 0;
  BrowserGpuTask next{[](void* data) { ++*static_cast<int*>(data); }, &calls};
  struct Context {
    BrowserGpuTaskQueue& queue;
    BrowserGpuTask& next;
  } context{queue, next};
  BrowserGpuTask first{[](void* data) {
                         auto& context = *static_cast<Context*>(data);
                         context.queue.enqueue(context.next);
                       },
                       &context};
  queue.enqueue(first);
  EXPECT_EQ(queue.executeBatch(), 1u);
  EXPECT_EQ(calls, 0);
  EXPECT_FALSE(queue.empty());
  EXPECT_EQ(queue.executeBatch(), 1u);
  EXPECT_EQ(calls, 1);
}

TEST(BrowserGpuTaskQueueTest, CallbackCanReclaimItsOwnBorrowedTask) {
  BrowserGpuTaskQueue queue;
  auto first = std::make_unique<BrowserGpuTask>();
  first->callback = [](void* data) {
    static_cast<std::unique_ptr<BrowserGpuTask>*>(data)->reset();
  };
  first->context = &first;
  bool secondRan = false;
  BrowserGpuTask second{[](void* data) { *static_cast<bool*>(data) = true; }, &secondRan};
  queue.enqueue(*first);
  queue.enqueue(second);
  EXPECT_EQ(queue.executeBatch(), 2u);
  EXPECT_FALSE(first);
  EXPECT_TRUE(secondRan);
}

}  // namespace
}  // namespace donner::gpu::browser
