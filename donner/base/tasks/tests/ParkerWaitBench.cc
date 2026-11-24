/// @file
/// Measures how long a parked thread takes to wake, for parker words the standard library waits on
/// natively and for words it waits on through its shared contention table. A performance probe,
/// not a correctness test: it prints a table and asserts nothing.
///
/// Each sample: a sleeper parks on its own word until a turn counter advances; the waker first lets
/// the sleeper settle into its platform wait (a busy delay longer than the library's spin phase),
/// then advances the turn and changes the word, notifying because the waiting bit is set. The
/// sample is the time from the change to the sleeper running again. A sample counts as waiting when
/// the change found the waiting bit set, so the sleeper had at least entered its wait; the table
/// reports how many samples were waiting and the latencies over those alone. One pair measures the
/// uncontended wake; several pairs at once, each on its own words, measure the wake while other
/// words are being waited on and notified.

#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/// The parker protocol on a word of type \p Word: waiting bit 0, epoch steps of 2, notify only when
/// the waiting bit was set. It mirrors the production parker so that any word type, including the
/// unsigned types the production parker rejects, can be measured the same way.
template <typename Word>
class alignas(128) ProbeParker {
public:
  template <typename Done>
  void parkUntil(Done&& done) {
    while (!done()) {
      const Word token = static_cast<Word>(word_.fetch_or(Word{1}, std::memory_order_seq_cst) | 1);
      if (done()) {
        word_.fetch_and(static_cast<Word>(~Word{1}), std::memory_order_relaxed);
        return;
      }
      word_.wait(token, std::memory_order_seq_cst);
      word_.fetch_and(static_cast<Word>(~Word{1}), std::memory_order_relaxed);
    }
  }

  /// @return True if the change found the waiting bit set.
  bool unpark() {
    if ((word_.fetch_add(Word{2}, std::memory_order_release) & 1) != 0) {
      word_.notify_one();
      return true;
    }
    return false;
  }

private:
  std::atomic<Word> word_{0};
};

/// One sleeper and its waker.
template <typename Word>
struct alignas(128) Pair {
  ProbeParker<Word> parker;
  alignas(128) std::atomic<int> turn{0};
  alignas(128) std::atomic<int> acknowledged{-1};
  std::vector<std::int64_t> wakeNs;
  std::vector<char> waiting;
};

/// Sorted wake latencies of a run: every sample, and the samples whose sleeper was waiting.
struct Latencies {
  std::vector<std::int64_t> all;
  std::vector<std::int64_t> waiting;
};

void BusyWait(std::chrono::nanoseconds duration) {
  const Clock::time_point until = Clock::now() + duration;
  while (Clock::now() < until) {}
}

std::int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

/// Runs \p pairs pairs at once for \p samples samples each. @return Every pair's wake latencies.
template <typename Word>
Latencies Measure(int pairs, int samples, std::chrono::nanoseconds settle) {
  std::vector<std::unique_ptr<Pair<Word>>> all;
  for (int i = 0; i < pairs; ++i) {
    all.push_back(std::make_unique<Pair<Word>>());
    all.back()->wakeNs.resize(static_cast<std::size_t>(samples));
    all.back()->waiting.resize(static_cast<std::size_t>(samples));
  }
  std::vector<std::int64_t> changeNs(static_cast<std::size_t>(pairs * samples));

  std::vector<std::thread> threads;
  for (int p = 0; p < pairs; ++p) {
    Pair<Word>& pair = *all[static_cast<std::size_t>(p)];
    threads.emplace_back([&pair, samples] {
      for (int i = 0; i < samples; ++i) {
        pair.parker.parkUntil([&] { return pair.turn.load(std::memory_order_acquire) > i; });
        pair.wakeNs[static_cast<std::size_t>(i)] = NowNs();
        pair.acknowledged.store(i, std::memory_order_release);
      }
    });
    threads.emplace_back([&pair, &changeNs, p, samples, settle] {
      for (int i = 0; i < samples; ++i) {
        while (pair.acknowledged.load(std::memory_order_acquire) != i - 1) {}
        BusyWait(settle);
        changeNs[static_cast<std::size_t>(p * samples + i)] = NowNs();
        pair.turn.store(i + 1, std::memory_order_release);
        pair.waiting[static_cast<std::size_t>(i)] = pair.parker.unpark() ? 1 : 0;
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  Latencies latencies;
  for (int p = 0; p < pairs; ++p) {
    const Pair<Word>& pair = *all[static_cast<std::size_t>(p)];
    for (int i = 0; i < samples; ++i) {
      const std::size_t sample = static_cast<std::size_t>(i);
      const std::int64_t latency =
          pair.wakeNs[sample] - changeNs[static_cast<std::size_t>(p * samples + i)];
      latencies.all.push_back(latency);
      if (pair.waiting[sample] != 0) {
        latencies.waiting.push_back(latency);
      }
    }
  }
  std::sort(latencies.all.begin(), latencies.all.end());
  std::sort(latencies.waiting.begin(), latencies.waiting.end());
  return latencies;
}

std::int64_t Percentile(const std::vector<std::int64_t>& sorted, double fraction) {
  if (sorted.empty()) {
    return -1;
  }
  return sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(fraction * sorted.size()))];
}

template <typename Word>
void Report(const char* wordName, int pairs, int samples, std::chrono::nanoseconds settle) {
  const Latencies latencies = Measure<Word>(pairs, samples, settle);
  const double waitingPercent =
      100.0 * static_cast<double>(latencies.waiting.size()) / latencies.all.size();
  std::printf("%-9s pairs=%-3d samples=%-6zu waiting=%5.1f%% p50_ns=%-8" PRId64 " p90_ns=%-8" PRId64
              " p99_ns=%-8" PRId64 " waiting_p50_ns=%-8" PRId64 " waiting_p90_ns=%-8" PRId64 "\n",
              wordName, pairs, latencies.all.size(), waitingPercent, Percentile(latencies.all, 0.5),
              Percentile(latencies.all, 0.9), Percentile(latencies.all, 0.99),
              Percentile(latencies.waiting, 0.5), Percentile(latencies.waiting, 0.9));
}

void PrintLoad(const char* when) {
  double load[3] = {0, 0, 0};
  if (getloadavg(load, 3) == 3) {
    std::printf("load %s: %.2f %.2f %.2f; hardware threads: %u\n", when, load[0], load[1], load[2],
                std::thread::hardware_concurrency());
  }
}

}  // namespace

int main(int argc, char** argv) {
  int samples = 2000;
  int manyPairs = 8;
  if (argc > 1) {
    samples = std::atoi(argv[1]);
  }
  if (argc > 2) {
    manyPairs = std::atoi(argv[2]);
  }
  const std::chrono::nanoseconds settle = std::chrono::microseconds(100);

  PrintLoad("before");
  for (const int pairs : {1, manyPairs}) {
    Report<std::uint32_t>("uint32_t", pairs, samples, settle);
    Report<std::int32_t>("int32_t", pairs, samples, settle);
    Report<std::uint64_t>("uint64_t", pairs, samples, settle);
    Report<std::int64_t>("int64_t", pairs, samples, settle);
  }
  PrintLoad("after");
  return 0;
}
