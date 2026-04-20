/**
 * @file
 * Drop-in `gtest_main` replacement that aborts any single test case that runs
 * longer than a per-case wall-clock budget. Hangs in GPU drivers, unresolvable
 * promises in an async runtime, or runaway loops trip a SIGALRM watchdog
 * instead of chewing up the whole Bazel test_timeout (which for a `size=large`
 * target is 15 minutes).
 *
 * Budget source (highest wins):
 *   1. `--donner_test_timeout_seconds=<N>` command-line flag.
 *   2. `DONNER_TEST_TIMEOUT_SECONDS` environment variable.
 *   3. Compile-time default of 30 s.
 *
 * On timeout, the process prints the offending test-case name to stderr and
 * exits with status 124 (the same code GNU `timeout(1)` uses). Bazel reports
 * it as a test failure with the name visible in the final line of output so
 * the culprit is obvious.
 */

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace {

// 60 s budget by default: tight enough to catch a real driver hang quickly
// but loose enough that slow-software-renderer paths (Mesa llvmpipe on CI,
// for example) can legitimately finish a heavy filter primitive without
// tripping the watchdog. Override with `--donner_test_timeout_seconds=N`
// or `DONNER_TEST_TIMEOUT_SECONDS=N` if a test is genuinely faster or
// needs even more slack.
constexpr unsigned int kDefaultTimeoutSeconds = 60;
constexpr int kTimeoutExitCode = 124;

// Ownership: set on the main thread in `OnTestStart`, read from the signal
// handler. A single test runs at a time under gtest, so no concurrent access.
// `std::string` storage is safe to `write()` from a signal handler as long as
// we don't mutate it there.
std::string gCurrentTestName;
pid_t gMainPid = 0;

void TimeoutHandler(int /*sig*/) {
  // Only the main test-runner process should trip the watchdog: gtest death
  // tests `fork()` into a child, and SIGALRM on the child should be a no-op
  // (the parent already has its own alarm governing the overall test).
  if (getpid() != gMainPid) {
    return;
  }

  constexpr std::string_view kPrefix = "\n[donner] test exceeded per-case timeout: ";
  (void)write(STDERR_FILENO, kPrefix.data(), kPrefix.size());
  (void)write(STDERR_FILENO, gCurrentTestName.data(), gCurrentTestName.size());
  (void)write(STDERR_FILENO, "\n", 1);
  _exit(kTimeoutExitCode);
}

unsigned int ResolveTimeoutSeconds(int argc, char** argv) {
  // Flag form: `--donner_test_timeout_seconds=<N>`. Consume it from argv so
  // it doesn't reach `InitGoogleTest` (which would complain about an unknown
  // flag).
  constexpr std::string_view kFlag = "--donner_test_timeout_seconds=";
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg.substr(0, kFlag.size()) == kFlag) {
      const int n = std::atoi(argv[i] + kFlag.size());
      for (int j = i; j + 1 < argc; ++j) {
        argv[j] = argv[j + 1];
      }
      argv[argc - 1] = nullptr;
      return n > 0 ? static_cast<unsigned int>(n) : kDefaultTimeoutSeconds;
    }
  }

  if (const char* env = std::getenv("DONNER_TEST_TIMEOUT_SECONDS")) {
    const int n = std::atoi(env);
    if (n > 0) {
      return static_cast<unsigned int>(n);
    }
  }

  return kDefaultTimeoutSeconds;
}

class TimeoutListener : public ::testing::EmptyTestEventListener {
 public:
  explicit TimeoutListener(unsigned int seconds) : seconds_(seconds) {}

  void OnTestStart(const ::testing::TestInfo& info) override {
    gCurrentTestName.assign(info.test_suite_name()).append(".").append(info.name());
    alarm(seconds_);
  }

  void OnTestEnd(const ::testing::TestInfo& /*info*/) override {
    alarm(0);
  }

 private:
  unsigned int seconds_;
};

}  // namespace

int main(int argc, char** argv) {
  // Compute the per-case budget BEFORE InitGoogleTest so our private flag gets
  // stripped out first.
  int mutable_argc = argc;
  const unsigned int seconds = ResolveTimeoutSeconds(mutable_argc, argv);
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

  ::testing::UnitTest::GetInstance()->listeners().Append(new TimeoutListener(seconds));

  return RUN_ALL_TESTS();
}
