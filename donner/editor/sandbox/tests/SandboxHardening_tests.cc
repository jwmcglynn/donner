/// @file
///
/// Tests for S6.1 sandbox hardening. These exercise two distinct layers:
///
/// 1. **In-process unit tests** — call `ApplyHardening()` directly to
///    verify the environment gate classifies correctly, and to confirm
///    setrlimit caps are actually installed (via `getrlimit` inspection).
/// 2. **Subprocess integration tests** — spawn the real child binary via
///    `fork` + `execve` with explicit environment control, to confirm the
///    gate fails closed when DONNER_SANDBOX is missing and works normally
///    when the host's curated envp is passed through. This is the only
///    layer where we can observe the child's exit code directly without
///    going through the `SandboxHost` pipe plumbing.

#include "donner/editor/sandbox/SandboxHardening.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "donner/base/tests/Runfiles.h"
#include "donner/editor/sandbox/SandboxProtocol.h"

extern "C" char** environ;

namespace donner::editor::sandbox {
namespace {

// -----------------------------------------------------------------------------
// In-process unit tests — call ApplyHardening() directly and check state.
// -----------------------------------------------------------------------------

TEST(SandboxHardeningUnitTest, MissingSandboxEnvFailsClosed) {
  // The test harness's environment may legitimately have DONNER_SANDBOX
  // set (or not). Strip it before calling ApplyHardening so the test is
  // deterministic regardless of how the suite was invoked.
  ::unsetenv("DONNER_SANDBOX");

  HardeningOptions opts;
  // Don't actually modify the test process's FDs or chdir — we only care
  // about the env gate classification here.
  opts.chdirRoot = false;
  opts.closeExtraFds = false;
  opts.addressSpaceBytes = 0;
  opts.cpuSeconds = 0;
  opts.maxFileBytes = 0;  // leave RLIMIT_FSIZE alone... oh wait, 0 means "block"
  opts.maxOpenFiles = 0;  // 0 means "leave RLIMIT_NOFILE alone"
  opts.logSummaryToStderr = false;
  opts.installSeccompFilter = false;  // don't jail the test process

  // maxFileBytes default is 0 which means "block all file writes" — we
  // can't actually call that inside a gtest process without breaking
  // stderr. For the unit test, set it to something large enough to be a
  // no-op but nonzero so the setrlimit path is still exercised, then
  // leave FD sweep and chdir off.
  opts.maxFileBytes = 1u << 30;  // 1 GiB cap, effectively unlimited for the test process

  const auto result = ApplyHardening(opts);
  EXPECT_EQ(result.status, HardeningStatus::kMissingSandboxEnv);
  EXPECT_FALSE(result.message.empty());
}

TEST(SandboxHardeningUnitTest, AppliesResourceLimitsWithSandboxEnv) {
  ::setenv("DONNER_SANDBOX", "1", /*overwrite=*/1);

  HardeningOptions opts;
  // Exercise the RLIMIT paths without actually hobbling the test process:
  // keep chdir and FD sweep off; set each cap high enough that real gtest
  // machinery is unaffected.
  opts.chdirRoot = false;
  opts.closeExtraFds = false;
  opts.addressSpaceBytes = 0;    // skip RLIMIT_AS
  opts.cpuSeconds = 0;           // skip RLIMIT_CPU
  opts.maxFileBytes = 1u << 30;  // 1 GiB cap — far above any test output
  opts.maxOpenFiles = 4096;      // generous
  opts.logSummaryToStderr = false;
  opts.installSeccompFilter = false;  // don't jail the test process

  const auto result = ApplyHardening(opts);
  EXPECT_EQ(result.status, HardeningStatus::kOk) << result.message;

  // Confirm at least one of the caps actually took effect by reading it
  // back via getrlimit.
  rlimit fsize{};
  ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &fsize), 0);
  EXPECT_LE(fsize.rlim_cur, static_cast<rlim_t>(1u << 30));

  rlimit nofile{};
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &nofile), 0);
  EXPECT_LE(nofile.rlim_cur, static_cast<rlim_t>(4096));
}

// -----------------------------------------------------------------------------
// Subprocess integration tests — spawn the real donner_parser_child with
// controlled envp and observe its exit code.
// -----------------------------------------------------------------------------

class HardenedChildTest : public ::testing::Test {
protected:
  void SetUp() override {
    // The child can exit before the parent finishes writing stdin (e.g. when
    // the hardening check rejects an empty envp in the "refuses" tests). Once
    // the child closes its read end, the parent's ::write() would raise
    // SIGPIPE and kill the gtest process mid-test, producing a bazel
    // "Broken pipe" failure with no diagnostics. Ignore SIGPIPE so writes
    // that race with a just-exited child return EPIPE instead.
    ::signal(SIGPIPE, SIG_IGN);
  }

  std::string ChildPath() {
    return Runfiles::instance().Rlocation("donner/editor/sandbox/donner_parser_child");
  }

  struct SpawnResult {
    int exitCode = 0;
    int termSignal = 0;
    std::string stderrCaptured;
  };

  // Spawns the child with the given envp, pipes `stdinBytes` to its stdin,
  // drains its stderr, reaps the child, and returns the exit info. stdout
  // is redirected to /dev/null so we don't have to decode the wire bytes
  // for tests that only care about the exit code.
  SpawnResult Spawn(std::string_view stdinBytes, const std::vector<std::string>& env) {
    SpawnResult out;

    int stdinFds[2] = {-1, -1};
    int stderrFds[2] = {-1, -1};
    ::pipe(stdinFds);
    ::pipe(stderrFds);

    const int devNull = ::open("/dev/null", O_WRONLY);
    EXPECT_GE(devNull, 0);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, stdinFds[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, devNull, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stderrFds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, stdinFds[0]);
    posix_spawn_file_actions_addclose(&actions, stdinFds[1]);
    posix_spawn_file_actions_addclose(&actions, stderrFds[0]);
    posix_spawn_file_actions_addclose(&actions, stderrFds[1]);
    posix_spawn_file_actions_addclose(&actions, devNull);

    const std::string childPath = ChildPath();
    std::string arg1 = "10";
    std::string arg2 = "10";
    std::array<char*, 4> argv = {
        const_cast<char*>(childPath.c_str()),
        arg1.data(),
        arg2.data(),
        nullptr,
    };

    // Build a mutable envp from the supplied strings.
    std::vector<std::string> envBacking = env;
    std::vector<char*> envp;
    envp.reserve(envBacking.size() + 1);
    for (auto& s : envBacking) envp.push_back(s.data());
    envp.push_back(nullptr);

    pid_t child = -1;
    const int rc = ::posix_spawn(&child, childPath.c_str(), &actions,
                                 /*attrp=*/nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    EXPECT_EQ(rc, 0);

    ::close(stdinFds[0]);
    ::close(stderrFds[1]);
    ::close(devNull);

    // Write stdin, close, drain stderr, reap. The child may have already
    // exited (and closed its stdin read end) by the time we get here — write
    // failures with EPIPE are expected in that case and intentionally
    // ignored. SIGPIPE is disabled in SetUp() so the signal does not kill
    // the test process.
    if (!stdinBytes.empty()) {
      const void* data = stdinBytes.data();
      std::size_t remaining = stdinBytes.size();
      while (remaining > 0) {
        const ssize_t n = ::write(stdinFds[1], data, remaining);
        if (n < 0) break;  // EPIPE, EINTR — child is gone or will be reaped below.
        remaining -= static_cast<std::size_t>(n);
        data = static_cast<const char*>(data) + n;
      }
    }
    ::close(stdinFds[1]);

    std::array<char, 4096> buf;
    while (true) {
      const ssize_t n = ::read(stderrFds[0], buf.data(), buf.size());
      if (n <= 0) break;
      out.stderrCaptured.append(buf.data(), static_cast<std::size_t>(n));
    }
    ::close(stderrFds[0]);

    int raw = 0;
    ::waitpid(child, &raw, 0);
    if (WIFEXITED(raw)) out.exitCode = WEXITSTATUS(raw);
    if (WIFSIGNALED(raw)) out.termSignal = WTERMSIG(raw);
    return out;
  }
};

TEST_F(HardenedChildTest, ChildRefusesWithoutSandboxEnvVar) {
  // Intentionally pass NO env vars. The child's first action is the
  // hardening check, which should exit 64 with a diagnostic.
  const auto result = Spawn("<svg/>", {});
  EXPECT_EQ(result.exitCode, kExitUsageError);
  EXPECT_EQ(result.termSignal, 0);
  EXPECT_NE(result.stderrCaptured.find("DONNER_SANDBOX=1"), std::string::npos)
      << "stderr was: " << result.stderrCaptured;
}

TEST_F(HardenedChildTest, ChildRefusesWhenSandboxEnvIsWrongValue) {
  const auto result = Spawn("<svg/>", {"DONNER_SANDBOX=0"});
  EXPECT_EQ(result.exitCode, kExitUsageError);
  EXPECT_NE(result.stderrCaptured.find("DONNER_SANDBOX=1"), std::string::npos);
}

TEST_F(HardenedChildTest, ChildRunsWithCuratedEnvpAndLogsProfile) {
#if !defined(__linux__)
  GTEST_SKIP() << "Subprocess hardening tests require Linux (seccomp, "
                  "close_range, minimal envp without DYLD)";
#endif
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10">
         <rect width="10" height="10" fill="red"/>
       </svg>)";
  const auto result = Spawn(kSvg, {"DONNER_SANDBOX=1"});
  // Successful render exits 0.
  EXPECT_EQ(result.exitCode, kExitOk) << "stderr: " << result.stderrCaptured;
  EXPECT_EQ(result.termSignal, 0);
  // Hardening logs its profile — grep for the stable prefix.
  EXPECT_NE(result.stderrCaptured.find("sandbox hardening:"), std::string::npos)
      << "expected hardening summary on stderr, got: " << result.stderrCaptured;
}

// ---------------------------------------------------------------------------
// Seccomp integration test: the child installs a seccomp-bpf filter by
// default. Verify that a normal SVG parse+render succeeds under the filter
// and that the log output confirms seccomp was enabled.
// ---------------------------------------------------------------------------

TEST_F(HardenedChildTest, ChildRendersSuccessfullyUnderSeccomp) {
#if !defined(__linux__)
  GTEST_SKIP() << "Seccomp is Linux-only; macOS child fails with minimal envp";
#endif
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20">
         <circle cx="10" cy="10" r="8" fill="blue" stroke="green" stroke-width="2"/>
       </svg>)";
  const auto result = Spawn(kSvg, {"DONNER_SANDBOX=1"});
  // The child must exit 0 — the seccomp filter must not block any syscall
  // that the normal parse+render path needs.
  EXPECT_EQ(result.exitCode, kExitOk)
      << "Child failed under seccomp filter. stderr: " << result.stderrCaptured;
  EXPECT_EQ(result.termSignal, 0) << "Child was killed by signal " << result.termSignal
                                  << " — possible seccomp KILL_PROCESS or missing allowlist entry. "
                                  << "stderr: " << result.stderrCaptured;

#if defined(__linux__)
  // On Linux, confirm the hardening summary includes seccomp=1 AND that
  // the deny action is `kill` — a regression back to `errno` would mean
  // a parser bug could silently succeed on a denied syscall.
  EXPECT_NE(result.stderrCaptured.find("seccomp=1"), std::string::npos)
      << "expected seccomp=1 in hardening summary, got: " << result.stderrCaptured;
  EXPECT_NE(result.stderrCaptured.find("seccomp_deny=kill"), std::string::npos)
      << "expected seccomp_deny=kill (production default) in hardening "
         "summary, got: "
      << result.stderrCaptured;
#endif
}

// ---------------------------------------------------------------------------
// Seccomp fail-closed deny-op probe (Linux-only, in-process via fork).
// Verifies that a denied syscall under the production filter terminates
// the child with SIGSYS (signal 31). Uses `fork()` + direct
// `ApplyHardening()` so we can pick the exact syscall to probe without
// teaching the real child binary a new code path.
// ---------------------------------------------------------------------------

#if defined(__linux__)

TEST(SandboxHardeningSeccompDenyOp, DeniedSyscallKillsChildWithSigsys) {
  pid_t child = ::fork();
  ASSERT_GE(child, 0) << "fork: " << std::strerror(errno);
  if (child == 0) {
    // Install the production seccomp filter with kKillProcess.
    HardeningOptions opts;
    opts.requireSandboxEnv = false;
    opts.chdirRoot = false;
    opts.closeExtraFds = false;
    opts.addressSpaceBytes = 0;
    opts.cpuSeconds = 0;
    opts.maxFileBytes = 1uLL << 30;
    opts.maxOpenFiles = 0;
    opts.logSummaryToStderr = false;
    opts.installSeccompFilter = true;
    opts.seccompDenyAction = HardeningOptions::SeccompDenyAction::kKillProcess;
    opts.installSandboxProfile = false;

    const auto result = ApplyHardening(opts);
    if (result.status != HardeningStatus::kOk) {
      _exit(64);  // hardening setup failed — not what we're testing
    }

    // `socket()` is not on the allowlist; under kKillProcess this must
    // terminate the process with SIGSYS. If we reach the `_exit(0)`
    // below, the filter is broken.
    (void)::socket(AF_INET, SOCK_STREAM, 0);
    _exit(0);
  }

  int status = 0;
  ASSERT_GE(::waitpid(child, &status, 0), 0);
  ASSERT_TRUE(WIFSIGNALED(status)) << "child exited normally with code " << WEXITSTATUS(status)
                                   << " — expected SIGSYS from seccomp kKillProcess";
  EXPECT_EQ(WTERMSIG(status), SIGSYS)
      << "child killed by signal " << WTERMSIG(status) << "; expected SIGSYS";
}

TEST(SandboxHardeningSeccompDenyOp, ErrnoModeReturnsEaccesInsteadOfKilling) {
  // Sanity check that the ad-hoc `kErrno` mode still works (for
  // developers extending the allowlist). Same syscall, different
  // deny action, must *not* SIGSYS-kill the child.
  pid_t child = ::fork();
  ASSERT_GE(child, 0) << "fork: " << std::strerror(errno);
  if (child == 0) {
    HardeningOptions opts;
    opts.requireSandboxEnv = false;
    opts.chdirRoot = false;
    opts.closeExtraFds = false;
    opts.addressSpaceBytes = 0;
    opts.cpuSeconds = 0;
    opts.maxFileBytes = 1uLL << 30;
    opts.maxOpenFiles = 0;
    opts.logSummaryToStderr = false;
    opts.installSeccompFilter = true;
    opts.seccompDenyAction = HardeningOptions::SeccompDenyAction::kErrno;
    opts.installSandboxProfile = false;

    const auto result = ApplyHardening(opts);
    if (result.status != HardeningStatus::kOk) _exit(64);

    errno = 0;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      ::close(fd);
      _exit(1);  // seccomp didn't block socket — test failure
    }
    // Any errno denying the call is fine; we just care that we didn't die.
    _exit(0);
  }

  int status = 0;
  ASSERT_GE(::waitpid(child, &status, 0), 0);
  ASSERT_TRUE(WIFEXITED(status)) << "child was signalled under kErrno mode; expected clean exit";
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

#endif  // __linux__

}  // namespace
}  // namespace donner::editor::sandbox
