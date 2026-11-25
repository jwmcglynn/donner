/// @file
/// Measures a parallel loop against the same inline work on a million independent items.

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <span>
#include <string_view>
#include <vector>

#include "donner/base/tasks/ParallelFor.h"
#include "donner/base/tasks/tests/SchedulerTestUtils.h"

namespace {

using donner::tasks::ParallelFor;
using donner::tasks::TaskScheduler;
using donner::tasks::TaskSchedulerTestPeer;
using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::duration<double, std::nano>;

constexpr std::size_t kItems = 1'000'000;
constexpr std::size_t kGrain = 256;
constexpr std::size_t kRepetitions = 5;
constexpr std::size_t kChildren = 64;

enum class Mode { Inline, Adaptive, SharedCursor };
constexpr std::array kModes = {Mode::Inline, Mode::Adaptive, Mode::SharedCursor};

/// Each item follows the same arithmetic dependency chain, regardless of its value or partition.
[[gnu::noinline]] void Transform(std::span<std::uint64_t> items, int rounds) {
  for (std::uint64_t& item : items) {
    std::uint64_t value = item;
    for (int round = 0; round < rounds; ++round) {
      value ^= value >> 27;
      value *= 0x9e3779b97f4a7c15ULL;
    }
    item = value;
  }
}

int CalibrateRounds() {
  constexpr int kProbeRounds = 128;
  constexpr double kTargetNs = 100.0;
  std::vector<std::uint64_t> probe(32'768);
  std::iota(probe.begin(), probe.end(), std::uint64_t{1});
  Transform(probe, kProbeRounds);
  const Clock::time_point start = Clock::now();
  Transform(probe, kProbeRounds);
  const double elapsedNs = Nanoseconds(Clock::now() - start).count();
  // Make the entire calibration loop observable without timing the checksum.
  std::printf("calibration_checksum=%" PRIu64 "\n",
              std::accumulate(probe.begin(), probe.end(), std::uint64_t{0}));
  const double desired = kProbeRounds * kTargetNs * probe.size() / std::max(1.0, elapsedNs);
  return static_cast<int>(std::clamp(desired + 0.5, 1.0, 4096.0));
}

void Run(Mode mode, TaskScheduler& scheduler, std::span<std::uint64_t> items, int rounds) {
  const auto body = [items, rounds](std::size_t first, std::size_t last) {
    Transform(items.subspan(first, last - first), rounds);
  };
  switch (mode) {
    case Mode::Inline: body(0, items.size()); break;
    case Mode::Adaptive: ParallelFor(scheduler, 0, items.size(), kGrain, body); break;
    case Mode::SharedCursor:
      donner::tasks::detail::ParallelForWithStrategy(
          donner::tasks::detail::ParallelForStrategy::SharedCursor, scheduler, 0, items.size(),
          kGrain, body);
      break;
  }
}

bool Measure(std::size_t workers, int rounds, std::span<std::uint64_t> items,
             std::span<const std::uint64_t> expected) {
  TaskScheduler scheduler({.workerCount = workers});
  std::array<std::array<double, kRepetitions>, kModes.size()> samples{};
  for (std::size_t rep = 0; rep < kRepetitions; ++rep) {
    for (std::size_t order = 0; order < kModes.size(); ++order) {
      const std::size_t index = (order + rep) % kModes.size();
      std::iota(items.begin(), items.end(), std::uint64_t{1});
      if (!donner::tasks::PollUntil([&] {
            return TaskSchedulerTestPeer::AwakeCount(scheduler) == 0 &&
                   TaskSchedulerTestPeer::AwakeStates(scheduler) == 0;
          })) {
        std::fprintf(stderr, "Workers did not reach their idle sleep before the sample\n");
        return false;
      }
      const Clock::time_point start = Clock::now();
      Run(kModes[index], scheduler, items, rounds);
      samples[index][rep] = Nanoseconds(Clock::now() - start).count() / kItems;
      const auto mismatch = std::mismatch(items.begin(), items.end(), expected.begin());
      if (mismatch.first != items.end()) {
        std::fprintf(stderr, "Incorrect output at item %zu\n",
                     static_cast<std::size_t>(mismatch.first - items.begin()));
        return false;
      }
    }
  }

  for (auto& times : samples) {
    std::sort(times.begin(), times.end());
  }
  constexpr std::array names = {"inline", "adaptive", "shared_cursor"};
  const double inlineNs = samples[0][kRepetitions / 2];
  for (std::size_t i = 0; i < names.size(); ++i) {
    const double median = samples[i][kRepetitions / 2];
    std::printf(
        "pool_workers=%zu mode=%s median_ns_per_item=%.2f speedup=%.2fx min_ns=%.2f max_ns=%.2f\n",
        workers, names[i], median, inlineNs / median, samples[i].front(), samples[i].back());
  }
  return true;
}

thread_local bool publishingChildren = false;

class WakeHandshakes : public donner::tasks::SchedulerTestHooks {
public:
  void at(donner::tasks::SchedulerStep step, std::size_t) override {
    if (step == donner::tasks::SchedulerStep::BetweenWakeSwapAndParkerChange) {
      total.fetch_add(1, std::memory_order_relaxed);
      if (publishingChildren) {
        producer.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  std::atomic<std::size_t> total{0};
  std::atomic<std::size_t> producer{0};
};

/// Count wake handshakes separately from timing. The hook observes the parker-change path even
/// when another thread won the state swap, so this count does not measure successful swaps.
bool MeasureFork(std::size_t workers, int rounds, std::span<std::uint64_t> items,
                 std::span<const std::uint64_t> expected) {
  WakeHandshakes hooks;
  auto scheduler = TaskSchedulerTestPeer::Make({.workerCount = workers}, &hooks);
  for (std::size_t rep = 0; rep < kRepetitions; ++rep) {
    std::iota(items.begin(), items.end(), std::uint64_t{1});
    bool isolated = false;
    std::size_t producer = 0;
    std::size_t allPublishing = 0;
    std::size_t allThroughJoin = 0;
    {
      donner::tasks::TaskScope root(*scheduler);
      root.spawn([&] {
        isolated = donner::tasks::PollUntil([&] {
          return TaskSchedulerTestPeer::AwakeCount(*scheduler) == 1 &&
                 TaskSchedulerTestPeer::AwakeStates(*scheduler) == 1 &&
                 TaskSchedulerTestPeer::SearchingCount(*scheduler) == 0;
        });
        if (!isolated) {
          return;
        }
        donner::tasks::TaskScope children(*scheduler);
        const std::size_t initialTotal = hooks.total.load(std::memory_order_relaxed);
        const std::size_t initialProducer = hooks.producer.load(std::memory_order_relaxed);
        publishingChildren = true;
        for (std::size_t child = 0; child < kChildren; ++child) {
          children.spawn([items, child, rounds] {
            constexpr std::size_t kItemsPerChild = kItems / kChildren;
            static_assert(kItems % kChildren == 0);
            Transform(items.subspan(child * kItemsPerChild, kItemsPerChild), rounds);
          });
        }
        publishingChildren = false;
        producer = hooks.producer.load(std::memory_order_relaxed) - initialProducer;
        allPublishing = hooks.total.load(std::memory_order_relaxed) - initialTotal;
        children.wait();
        allThroughJoin = hooks.total.load(std::memory_order_relaxed) - initialTotal;
      });
    }
    if (!isolated || !std::equal(items.begin(), items.end(), expected.begin())) {
      std::fprintf(stderr, "Fork probe failed setup or output validation at %zu workers\n",
                   workers);
      return false;
    }
    std::printf(
        "pool_workers=%zu fork_children=%zu sample=%zu producer_handshakes=%zu "
        "all_during_publish=%zu all_through_join=%zu\n",
        workers, kChildren, rep + 1, producer, allPublishing, allThroughJoin);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  int rounds = 0;
  if (argc == 1) {
    rounds = CalibrateRounds();
  } else if (argc == 2) {
    const std::string_view text(argv[1]);
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), rounds);
    if (error != std::errc{} || end != text.data() + text.size() || rounds < 1 || rounds > 4096) {
      std::fprintf(stderr, "Expected a round count between 1 and 4096\n");
      return 1;
    }
  } else {
    std::fprintf(stderr, "Usage: parallel_for_scaling_bench [rounds]\n");
    return 1;
  }
  std::vector<std::uint64_t> expected(kItems);
  std::iota(expected.begin(), expected.end(), std::uint64_t{1});
  Transform(expected, rounds);
  std::vector<std::uint64_t> items(kItems);
  std::printf("items=%zu grain=%zu rounds=%d samples=%zu\n", kItems, kGrain, rounds, kRepetitions);
  for (std::size_t workers : {4u, 8u, 16u}) {
    if (!Measure(workers, rounds, items, expected) ||
        !MeasureFork(workers, rounds, items, expected)) {
      return 1;
    }
  }
  return 0;
}
