/// @file
/// Measures a parallel loop against the same inline work on a million independent items.

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <span>
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

}  // namespace

int main() {
  const int rounds = CalibrateRounds();
  std::vector<std::uint64_t> expected(kItems);
  std::iota(expected.begin(), expected.end(), std::uint64_t{1});
  Transform(expected, rounds);
  std::vector<std::uint64_t> items(kItems);
  TaskScheduler scheduler({.workerCount = 4});
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
        return 1;
      }
      const Clock::time_point start = Clock::now();
      Run(kModes[index], scheduler, items, rounds);
      samples[index][rep] = Nanoseconds(Clock::now() - start).count() / kItems;
      const auto mismatch = std::mismatch(items.begin(), items.end(), expected.begin());
      if (mismatch.first != items.end()) {
        std::fprintf(stderr, "Incorrect output at item %zu\n",
                     static_cast<std::size_t>(mismatch.first - items.begin()));
        return 1;
      }
    }
  }

  for (auto& times : samples) {
    std::sort(times.begin(), times.end());
  }
  constexpr std::array names = {"inline", "adaptive", "shared_cursor"};
  const double inlineNs = samples[0][kRepetitions / 2];
  std::printf("items=%zu grain=%zu rounds=%d samples=%zu\n", kItems, kGrain, rounds, kRepetitions);
  for (std::size_t i = 0; i < names.size(); ++i) {
    const double median = samples[i][kRepetitions / 2];
    std::printf(
        "mode=%s workers=%d median_ns_per_item=%.2f speedup=%.2fx min_ns=%.2f max_ns=%.2f\n",
        names[i], i == 0 ? 0 : 4, median, inlineNs / median, samples[i].front(), samples[i].back());
  }
  return 0;
}
