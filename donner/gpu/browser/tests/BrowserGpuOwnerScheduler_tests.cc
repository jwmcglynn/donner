#include "donner/gpu/browser/BrowserGpuOwnerScheduler.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <ostream>
#include <vector>

namespace donner::gpu::browser {
namespace {

using testing::Each;
using testing::ElementsAre;
using testing::Field;
using testing::IsEmpty;
using testing::SizeIs;

/// One callback start the owner made.
struct Start {
  int request = 0;            //!< Request identity.
  bool canvasActive = false;  //!< Whether a canvas frame was active when it started.
  bool runtimeWait = false;   //!< Whether a runtime wait started it.

  friend std::ostream& operator<<(std::ostream& os, const Start& start) {
    return os << "{request=" << start.request << " canvasActive=" << start.canvasActive
              << " runtimeWait=" << start.runtimeWait << "}";
  }
};

/// A single-threaded owner: proxied requests wait in a mailbox until the owner delivers them, and
/// a scheduled pulse waits until the test delivers it, as the owner's event loop would.
class FakeOwner final : public BrowserGpuOwnerScheduler::Host {
public:
  double nowMs() override { return clockMs; }
  void schedulePulse() override {
    EXPECT_FALSE(pulsePending) << "the scheduler asked for a second outstanding pulse";
    pulsePending = true;
  }
  void deliverPendingRequests() override {
    // Requests posted while this delivery runs arrive with the next one.
    std::deque<BrowserGpuTask*> delivered;
    delivered.swap(mailbox);
    for (BrowserGpuTask* task : delivered) {
      scheduler.enqueue(*task);
    }
  }
  void recordBatch(std::size_t, double) override {}
  void recordInline(std::size_t count, double, std::size_t epochCalls) override {
    inlineStarts += count;
    maximumEpochCalls = std::max(maximumEpochCalls, epochCalls);
  }
  void recordPulse(std::size_t count) override { pulseStarts += count; }

  /// The owner's event loop runs one proxied delivery message.
  void runDeliveryMessage() { deliverPendingRequests(); }

  /// The owner's event loop runs the pulse message, when one was posted.
  void runPulseMessage() {
    if (pulsePending) {
      pulsePending = false;
      scheduler.pulse();
    }
  }

  /// The owner blocks in a runtime wait slice.
  void runRuntimeWait() {
    inRuntimeWait = true;
    scheduler.serviceWait();
    inRuntimeWait = false;
  }

  /// One canvas frame from before acquisition to after retirement, with nothing delivered inside.
  void runCanvasFrame() {
    scheduler.setCanvasFrameActive(true);
    scheduler.setCanvasFrameActive(false);
  }

  double clockMs = 0.0;
  bool pulsePending = false;
  bool inRuntimeWait = false;
  std::size_t inlineStarts = 0;
  std::size_t pulseStarts = 0;
  std::size_t maximumEpochCalls = 0;
  std::deque<BrowserGpuTask*> mailbox;
  std::vector<Start> starts;
  BrowserGpuOwnerScheduler scheduler{*this};
};

/// A thread that proxies one blocking request at a time, posting the next once the previous ends.
class SequentialClient {
public:
  /// @param owner Owner the requests are proxied to. @param firstId Identity of the first request.
  /// @param count Requests to issue. @param costMs Owner time each callback takes.
  SequentialClient(FakeOwner& owner, int firstId, std::size_t count, double costMs)
      : owner_(owner), requests_(count) {
    for (std::size_t index = 0; index < count; ++index) {
      Request& request = requests_[index];
      request.client = this;
      request.id = firstId + static_cast<int>(index);
      request.costMs = costMs;
      request.task.callback = &Run;
      request.task.context = &request;
    }
  }

  SequentialClient(const SequentialClient&) = delete;
  SequentialClient& operator=(const SequentialClient&) = delete;

  /// Post the first request.
  void start() { post(0); }

  /// Requests whose callbacks finished.
  [[nodiscard]] std::size_t completed() const { return completed_; }

private:
  struct Request {
    SequentialClient* client = nullptr;
    int id = 0;
    double costMs = 0.0;
    BrowserGpuTask task;
  };

  void post(std::size_t index) {
    if (index < requests_.size()) {
      owner_.mailbox.push_back(&requests_[index].task);
    }
  }

  static void Run(void* context) {
    auto& request = *static_cast<Request*>(context);
    SequentialClient& client = *request.client;
    client.owner_.starts.push_back(
        Start{.request = request.id,
              .canvasActive = client.owner_.scheduler.canvasFrameActive(),
              .runtimeWait = client.owner_.inRuntimeWait});
    client.owner_.clockMs += request.costMs;
    // The blocked thread wakes and proxies its next request; the owner sees it on a later delivery.
    client.post(++client.completed_);
  }

  FakeOwner& owner_;
  std::vector<Request> requests_;
  std::size_t completed_ = 0;
};

TEST(BrowserGpuOwnerScheduler, StartsALoneRequestInlineAndKeepsAPulseForRenewal) {
  FakeOwner owner;
  SequentialClient client(owner, 1, 1, 0.01);
  client.start();
  owner.runDeliveryMessage();

  EXPECT_THAT(owner.starts, ElementsAre(Field(&Start::request, 1)));
  EXPECT_EQ(owner.inlineStarts, 1u);
  EXPECT_TRUE(owner.pulsePending) << "only a later pulse renews the inline budget";
}

TEST(BrowserGpuOwnerScheduler, InlineStartsStopAtTheCallBudgetUntilAPulse) {
  FakeOwner owner;
  SequentialClient client(owner, 1, BrowserGpuOwnerScheduler::kMaximumInlineCalls + 1, 0.0);
  client.start();
  for (std::size_t message = 0; message <= BrowserGpuOwnerScheduler::kMaximumInlineCalls;
       ++message) {
    owner.runDeliveryMessage();
  }
  EXPECT_THAT(owner.starts, SizeIs(BrowserGpuOwnerScheduler::kMaximumInlineCalls));
  EXPECT_EQ(owner.maximumEpochCalls, BrowserGpuOwnerScheduler::kMaximumInlineCalls);

  owner.runPulseMessage();
  EXPECT_EQ(client.completed(), BrowserGpuOwnerScheduler::kMaximumInlineCalls + 1);
  EXPECT_EQ(owner.pulseStarts, 1u);
}

TEST(BrowserGpuOwnerScheduler, InlineStartsStopAtTheTimeBudgetUntilAPulse) {
  FakeOwner owner;
  SequentialClient client(owner, 1, 8, 0.75);
  client.start();
  for (int message = 0; message < 8; ++message) {
    owner.runDeliveryMessage();
  }
  // Starts at 0, 0.75 and 1.5 ms of callback time; the third ends past the 2 ms budget.
  EXPECT_EQ(client.completed(), 3u);

  owner.runPulseMessage();
  EXPECT_EQ(client.completed(), 4u);
}

TEST(BrowserGpuOwnerScheduler, ACanvasFrameDefersInlineStartsAndPulses) {
  FakeOwner owner;
  SequentialClient client(owner, 1, 1, 0.01);
  owner.scheduler.setCanvasFrameActive(true);
  client.start();
  owner.runDeliveryMessage();
  EXPECT_THAT(owner.starts, IsEmpty());
  EXPECT_FALSE(owner.pulsePending) << "a pulse was posted inside a canvas frame";

  owner.scheduler.setCanvasFrameActive(false);
  owner.runPulseMessage();
  EXPECT_THAT(owner.starts, ElementsAre(Field(&Start::canvasActive, false)));
}

TEST(BrowserGpuOwnerScheduler, RuntimeWaitsKeepDependenciesProgressingInsideACanvasFrame) {
  FakeOwner owner;
  SequentialClient client(owner, 1, 2, 0.01);
  owner.scheduler.setCanvasFrameActive(true);
  client.start();
  owner.runRuntimeWait();
  owner.runRuntimeWait();
  owner.scheduler.setCanvasFrameActive(false);

  EXPECT_THAT(owner.starts,
              ElementsAre(Field(&Start::runtimeWait, true), Field(&Start::runtimeWait, true)));
  EXPECT_EQ(owner.inlineStarts, 0u);
}

TEST(BrowserGpuOwnerScheduler, ReportsIdleOnlyWithNothingQueuedOrActive) {
  FakeOwner owner;
  EXPECT_TRUE(owner.scheduler.idle());
  owner.scheduler.setCanvasFrameActive(true);
  EXPECT_FALSE(owner.scheduler.idle()) << "a canvas frame is active";
  owner.scheduler.setCanvasFrameActive(false);
  EXPECT_TRUE(owner.scheduler.idle());

  SequentialClient client(owner, 1, BrowserGpuOwnerScheduler::kMaximumInlineCalls + 1, 0.0);
  client.start();
  for (std::size_t message = 0; message <= BrowserGpuOwnerScheduler::kMaximumInlineCalls;
       ++message) {
    owner.runDeliveryMessage();
  }
  EXPECT_FALSE(owner.scheduler.idle()) << "a request beyond the inline budget is still queued";
  owner.runPulseMessage();
  EXPECT_TRUE(owner.scheduler.idle());
}

}  // namespace
}  // namespace donner::gpu::browser
