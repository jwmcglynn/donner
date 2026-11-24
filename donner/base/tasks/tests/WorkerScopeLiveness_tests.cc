/// @file
/// The last-awake decision for nested stolen joins racing an outside publication.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <memory>

#include "donner/base/tasks/TaskScope.h"
#include "donner/base/tasks/tests/SchedulerTestUtils.h"

namespace donner::tasks {
namespace {

using Peer = TaskSchedulerTestPeer;

/// Each worker's first idle sleep is held so a local fork can be stolen without a timing race.
struct NestedJoinHooks : SchedulerTestHooks {
  void at(SchedulerStep step, std::size_t worker) override {
    if (step == SchedulerStep::AfterSleeperDecrement) {
      if (!started[worker]) {
        started[worker] = true;
        firstSleep[worker].arriveAndWait();
      } else if (worker == 0) {
        outerAsleep.arriveAndWait();
      } else if (worker == 1) {
        innerAsleep.arriveAndWait();
      }
    } else if (step == SchedulerStep::AfterWaitingBit && worker == 1 && started[worker]) {
      innerPrepared.arriveAndWait();
    } else if (step == SchedulerStep::BeforeSleeperWaits && worker == 2) {
      leafAsleep.arriveAndWait();
    } else if (step == SchedulerStep::BeforeHandOff) {
      handOffs.fetch_add(1);
    }
  }

  void release() {
    for (Gate& gate : firstSleep) {
      gate.release();
    }
    innerPrepared.release();
    innerAsleep.release();
    outerAsleep.release();
    leafAsleep.release();
  }

  std::array<bool, 3> started{};  //!< Each element is accessed only by its worker's sleep hooks.
  std::array<Gate, 3> firstSleep;
  Gate innerPrepared;
  Gate innerAsleep;
  Gate outerAsleep;
  Gate leafAsleep;
  std::atomic<int> handOffs{0};
};

class NestedJoinRace {
public:
  NestedJoinRace()
      : scheduler_(Peer::Make(
            TaskSchedulerOptions{.workerCount = 3,
                                 .helpMarginBytes = std::numeric_limits<std::size_t>::max()},
            &hooks_)),
        outside_(*scheduler_) {}

  ~NestedJoinRace() {
    hooks_.release();
    outerFork_.release();
    innerFork_.release();
    leaf_.release();
    outside_.wait();
  }

  /// Worker zero forks to worker one, which forks to worker two. Worker two finishes the inner
  /// join before worker one stores its sleep state. The still-incomplete outer join can then
  /// enter deep sleep as the only counted worker.
  testing::AssertionResult arrange() {
    for (Gate& gate : hooks_.firstSleep) {
      if (!gate.waitForArrival()) {
        return testing::AssertionFailure() << "workers did not reach their first sleep";
      }
    }
    outside_.spawn([this] {
      Join(
          *scheduler_, [this] { outerFork_.arriveAndWait(); },
          [this] {
            Join(
                *scheduler_, [this] { innerFork_.arriveAndWait(); },
                [this] { leaf_.arriveAndWait(); });
          });
    });
    hooks_.firstSleep[0].release();
    if (!outerFork_.waitForArrival()) {
      return testing::AssertionFailure() << "outer join did not fork";
    }
    // A global entry lets each idle thief pass its visibility recheck and search again.
    outside_.spawn([] {});
    hooks_.firstSleep[1].release();
    if (!innerFork_.waitForArrival()) {
      return testing::AssertionFailure() << "first thief did not fork the inner task";
    }
    outside_.spawn([] {});
    hooks_.firstSleep[2].release();
    if (!leaf_.waitForArrival()) {
      return testing::AssertionFailure() << "second thief did not take the inner task";
    }
    innerFork_.release();
    if (!hooks_.innerPrepared.waitForArrival()) {
      return testing::AssertionFailure() << "inner waiter did not prepare its parker";
    }
    leaf_.release();
    if (!hooks_.leafAsleep.waitForArrival()) {
      return testing::AssertionFailure() << "inner finisher did not return to an idle sleep";
    }
    hooks_.innerPrepared.release();
    if (!hooks_.innerAsleep.waitForArrival()) {
      return testing::AssertionFailure() << "inner waiter did not decrement before its recheck";
    }
    return testing::AssertionSuccess();
  }

  testing::AssertionResult run(bool publishBeforeDecrement) {
    const auto prepared = arrange();
    if (!prepared) {
      return prepared;
    }
    if (publishBeforeDecrement) {
      publish();
    }
    outerFork_.release();
    if (!hooks_.outerAsleep.waitForArrival()) {
      return testing::AssertionFailure() << "outer waiter did not decrement";
    }
    if (publishBeforeDecrement) {
      // No publisher woke worker two. Its next wake must come from the outer waiter.
      hooks_.leafAsleep.release();
    } else {
      // The publisher now wakes worker two. Hold it before the wait so the outer waiter can
      // take the queued work after finding no shallow sleeper.
      publish();
    }
    hooks_.outerAsleep.release();
    if (!publishedRan_.wait()) {
      return testing::AssertionFailure() << "the outside task did not run";
    }
    const int expectedWorker = publishBeforeDecrement ? 2 : 0;
    EXPECT_THAT(publishedWorker_.load(), testing::Eq(expectedWorker));
    EXPECT_THAT(hooks_.handOffs.load(), testing::Eq(publishBeforeDecrement ? 1 : 0));
    EXPECT_THAT(Peer::PastMarginRuns(*scheduler_), testing::Eq(publishBeforeDecrement ? 0u : 1u));
    return testing::AssertionSuccess();
  }

private:
  void publish() {
    outside_.spawn([this] {
      publishedWorker_.store(Peer::WorkerIndex(*scheduler_));
      publishedRan_.set();
    });
  }

  NestedJoinHooks hooks_;
  Gate outerFork_;
  Gate innerFork_;
  Gate leaf_;
  Event publishedRan_;
  std::atomic<int> publishedWorker_{-1};
  std::unique_ptr<TaskScheduler> scheduler_;
  TaskScope outside_;
};

TEST(WorkerNestedJoinTest, PublicationBeforeTheLastDecrementHandsOffToTheShallowWorker) {
  NestedJoinRace race;
  EXPECT_TRUE(race.run(true));
}

TEST(WorkerNestedJoinTest, PublicationAfterTheLastDecrementLetsTheDeepWorkerTakeTheTask) {
  NestedJoinRace race;
  EXPECT_TRUE(race.run(false));
}

}  // namespace
}  // namespace donner::tasks
