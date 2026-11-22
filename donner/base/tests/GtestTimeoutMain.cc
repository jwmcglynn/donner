/**
 * @file
 * Drop-in `gtest_main` replacement that aborts a test case that hangs, naming the case, before
 * Bazel's own test timeout can kill the process anonymously. Hangs in GPU drivers, unresolvable
 * promises in an async runtime, or runaway loops trip a SIGALRM watchdog instead of running into
 * the target timeout with no indication of which case stalled.
 *
 * Two limits apply, and the earlier one wins:
 *   1. The per-case budget, from (highest wins) the `--donner_test_timeout_seconds=<N>` flag, the
 *      `DONNER_TEST_TIMEOUT_SECONDS` environment variable, or a default of 60 s.
 *   2. A deadline derived from Bazel's `TEST_TIMEOUT`, ending a fixed margin before the Bazel
 *      timeout and measured from `main()` entry (see `GtestTimeoutBudget.h`). It bounds the whole
 *      run, including time spent in earlier cases and outside any case, so the watchdog fires first
 * in every test size, shard and
 *      `--test_timeout` setting without per-target flags.
 *
 * On timeout, the process prints the stalled test-case name to stderr and exits with status 124
 * (the same code GNU `timeout(1)` uses).
 */

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>  // IWYU pragma: keep - provides pid_t (clang-tidy-19 mapping)
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "donner/base/FailureSignalHandler.h"
#include "donner/base/tests/GtestTimeoutBudget.h"

namespace {

using std::chrono::milliseconds;

constexpr int kTimeoutExitCode = 124;

// The message for the armed alarm, in static storage so the handler never follows a pointer that
// could be freed. SIGALRM is process-directed, so the handler can run on any thread. The main
// thread rewrites the message only after disarming the timer; the one possible overlap is an alarm
// that expired just before that disarm being handled on another thread, and then the text may mix
// the old and new message. The handler still reads only inside this buffer and still exits 124.
char gTimeoutMessage[1024];
std::atomic<std::size_t> gTimeoutMessageSize{0};
static_assert(std::atomic<std::size_t>::is_always_lock_free,
              "the signal handler reads the message size");
// pid_t canonical header differs between clang-tidy-18 (<sys/types.h>) and
// clang-tidy-19 (which doesn't map it to anything currently included). The
// trailing NOLINT suppresses the latter without chasing mapping deltas.
pid_t gMainPid = 0;  // NOLINT(misc-include-cleaner)

void TimeoutHandler(int /*sig*/) {
  // Only the main test-runner process should trip the watchdog: gtest death
  // tests `fork()` into a child, and SIGALRM on the child should be a no-op
  // (the parent already has its own alarm governing the overall test).
  if (getpid() != gMainPid) {
    return;
  }

  (void)write(STDERR_FILENO, gTimeoutMessage, gTimeoutMessageSize.load());
  _exit(kTimeoutExitCode);
}

void SetTimeoutMessage(const std::string& message) {
  const std::size_t size = std::min(message.size(), sizeof(gTimeoutMessage));
  std::memcpy(gTimeoutMessage, message.data(), size);
  gTimeoutMessageSize.store(size);
}

void SetTimer(milliseconds interval) {
  struct itimerval timer = {};
  timer.it_value.tv_sec = static_cast<time_t>(interval.count() / 1000);
  timer.it_value.tv_usec = static_cast<suseconds_t>((interval.count() % 1000) * 1000);
  setitimer(ITIMER_REAL, &timer, nullptr);
}

std::optional<milliseconds> ResolvePerCaseBudget(int argc, char** argv) {
  // Flag form: `--donner_test_timeout_seconds=<N>`. Consume it from argv so
  // it doesn't reach `InitGoogleTest` (which would complain about an unknown
  // flag).
  constexpr std::string_view kFlag = "--donner_test_timeout_seconds=";
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg.substr(0, kFlag.size()) == kFlag) {
      for (int j = i; j + 1 < argc; ++j) {
        argv[j] = argv[j + 1];
      }
      argv[argc - 1] = nullptr;
      return donner::tests::ParsePositiveSeconds(arg.substr(kFlag.size()));
    }
  }

  if (const char* env = std::getenv("DONNER_TEST_TIMEOUT_SECONDS")) {
    return donner::tests::ParsePositiveSeconds(env);
  }
  return std::nullopt;
}

/// Arms the watchdog for the next stretch of the run, which is either one test case (with a
/// per-case budget) or the time outside any case (deadline only).
class Watchdog {
public:
  Watchdog(milliseconds perCaseBudget, std::optional<milliseconds> deadline,
           std::chrono::steady_clock::time_point mainEntry)
      : perCaseBudget_(perCaseBudget), deadline_(deadline), mainEntry_(mainEntry) {}

  /**
   * Replaces the pending alarm. The message records when the alarm was armed and for how long,
   * so a deadline alarm reports `armed + interval == deadline`.
   *
   * @param where Names the stretch in the timeout message.
   * @param inTestCase Whether the per-case budget applies.
   */
  void arm(const std::string& where, bool inTestCase) {
    SetTimer(milliseconds(0));

    const auto elapsed =
        std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - mainEntry_);
    const std::optional<milliseconds> budget =
        inTestCase ? std::optional<milliseconds>(perCaseBudget_) : std::nullopt;
    const milliseconds interval = donner::tests::WatchdogAlarmInterval(budget, deadline_, elapsed);
    if (interval == milliseconds(0)) {
      return;
    }

    const std::string timing = " (alarm armed " + std::to_string(elapsed.count()) +
                               " ms after main() for " + std::to_string(interval.count()) + " ms";
    if (budget && interval == *budget) {
      SetTimeoutMessage("\n[donner] test exceeded per-case timeout: " + where + timing + ")\n");
    } else {
      SetTimeoutMessage(
          "\n[donner] test reached the watchdog deadline ahead of Bazel's TEST_TIMEOUT: " + where +
          timing + "; deadline " + std::to_string(deadline_->count()) + " ms after main())\n");
    }
    SetTimer(interval);
  }

private:
  milliseconds perCaseBudget_;
  std::optional<milliseconds> deadline_;
  std::chrono::steady_clock::time_point mainEntry_;
};

class TimeoutListener : public ::testing::EmptyTestEventListener {
public:
  explicit TimeoutListener(Watchdog watchdog) : watchdog_(watchdog) {}

  void OnTestSuiteStart(const ::testing::TestSuite& suite) override {
    watchdog_.arm(std::string("outside any test case, in setup of test suite ") + suite.name(),
                  /*inTestCase=*/false);
  }

  void OnTestStart(const ::testing::TestInfo& info) override {
    watchdog_.arm(std::string(info.test_suite_name()) + "." + info.name(), /*inTestCase=*/true);
  }

  void OnTestEnd(const ::testing::TestInfo& info) override {
    watchdog_.arm(
        std::string("outside any test case, after ") + info.test_suite_name() + "." + info.name(),
        /*inTestCase=*/false);
  }

private:
  Watchdog watchdog_;
};

}  // namespace

int main(int argc, char** argv) {
  const auto mainEntry = std::chrono::steady_clock::now();
  donner::InstallFailureSignalHandler();

  // Compute the per-case budget BEFORE InitGoogleTest so our private flag gets
  // stripped out first.
  int mutable_argc = argc;
  const milliseconds perCaseBudget =
      ResolvePerCaseBudget(mutable_argc, argv).value_or(donner::tests::kDefaultPerCaseBudget);
  // Rebuild argc after flag stripping so gtest sees the correct count.
  while (mutable_argc > 0 && argv[mutable_argc - 1] == nullptr) {
    --mutable_argc;
  }

  ::testing::InitGoogleTest(&mutable_argc, argv);

  gMainPid = getpid();
  struct sigaction sa = {};
  sa.sa_handler = &TimeoutHandler;
  sigemptyset(&sa.sa_mask);
  // No SA_RESTART: we WANT syscalls interrupted by the handler so they
  // unwind fast after _exit().
  sa.sa_flags = 0;
  sigaction(SIGALRM, &sa, nullptr);

  Watchdog watchdog(perCaseBudget, donner::tests::WatchdogDeadline(std::getenv("TEST_TIMEOUT")),
                    mainEntry);
  watchdog.arm("outside any test case, before the first case", /*inTestCase=*/false);
  ::testing::UnitTest::GetInstance()->listeners().Append(new TimeoutListener(watchdog));

  return RUN_ALL_TESTS();
}
