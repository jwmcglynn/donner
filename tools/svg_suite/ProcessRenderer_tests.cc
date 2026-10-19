/// @file
/// Child-process failure, deadline, and argv boundaries exercised with this test executable.

#include "tools/svg_suite/ProcessRenderer.h"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "tools/svg_suite/ProcessRendererInternal.h"

namespace donner::svg_suite {
namespace {

std::filesystem::path executable;

// A descendant keeps the write end open until it exits. EOF proves termination without relying
// on process-list timing or on an orphan zombie being promptly reaped by the host.
class DescendantProbe {
public:
  bool open() { return pipe(descriptors_) == 0; }

  ~DescendantProbe() {
    if (descendant_ > 0 && !stopped_) {
      // A broken cleanup implementation must not leave the regression fixture running.
      kill(descendant_, SIGKILL);
      pollfd descriptor{descriptors_[0], POLLIN | POLLHUP, 0};
      poll(&descriptor, 1, 1000);
    }
    for (const int descriptor : descriptors_) {
      if (descriptor >= 0) {
        close(descriptor);
      }
    }
  }

  std::string mode(std::string_view outcome) const {
    return "descendant-" + std::string(outcome) + ":" + std::to_string(descriptors_[1]);
  }

  bool stopped() {
    close(descriptors_[1]);
    descriptors_[1] = -1;
    pollfd descriptor{descriptors_[0], POLLIN | POLLHUP, 0};
    if (poll(&descriptor, 1, 1000) <= 0 ||
        read(descriptors_[0], &descendant_, sizeof(descendant_)) !=
            static_cast<ssize_t>(sizeof(descendant_)) ||
        descendant_ <= 0) {
      return false;
    }
    if (poll(&descriptor, 1, 1000) <= 0) {
      return false;
    }
    char unexpected;
    stopped_ = read(descriptors_[0], &unexpected, 1) == 0;
    return stopped_;
  }

private:
  int descriptors_[2] = {-1, -1};
  pid_t descendant_ = -1;
  bool stopped_ = false;
};

class ScopedChildDisposition {
public:
  ScopedChildDisposition() = default;
  ScopedChildDisposition(const ScopedChildDisposition&) = delete;
  ScopedChildDisposition& operator=(const ScopedChildDisposition&) = delete;
  ~ScopedChildDisposition() { restore(); }

  bool install(void (*handler)(int), int flags) {
    struct sigaction replacement{};
    replacement.sa_handler = handler;
    replacement.sa_flags = flags;
    sigemptyset(&replacement.sa_mask);
    active_ = sigaction(SIGCHLD, &replacement, &previous_) == 0;
    return active_;
  }

  bool restore() {
    if (!active_) {
      return true;
    }
    if (sigaction(SIGCHLD, &previous_, nullptr) != 0) {
      return false;
    }
    active_ = false;
    return true;
  }

private:
  struct sigaction previous_{};
  bool active_ = false;
};

void NotifyChild(int) {}

bool SameChildDisposition(const struct sigaction& before) {
  struct sigaction after{};
  return sigaction(SIGCHLD, nullptr, &after) == 0 && before.sa_handler == after.sa_handler &&
         before.sa_flags == after.sa_flags;
}

int CheckPolicyFixtureOutcome(std::string_view mode, const ImageResult& result,
                              const std::filesystem::path& root) {
  if (mode == "notify") {
    return result.image && result.error.empty() ? 0 : 14;
  }
  if (result.image || result.error.find("SIGCHLD") == std::string::npos ||
      std::filesystem::exists(root / "fixture/process/render.png")) {
    return 15;
  }
  return 0;
}

int ReapingPolicyFixture(std::string_view mode, const std::filesystem::path& root) {
  ScopedChildDisposition disposition;
  const auto handler = mode == "ignored" ? SIG_IGN : mode == "notify" ? NotifyChild : SIG_DFL;
  const int flags = mode == "auto-reap" ? SA_NOCLDWAIT : 0;
  if (!disposition.install(handler, flags)) {
    return 10;
  }
  struct sigaction before{};
  if (sigaction(SIGCHLD, nullptr, &before) != 0) {
    return 11;
  }
  const StaticCase test{"fixture", root, root / "input {root} {output} ; literal.svg",
                        root / "expected.png"};
  const auto renderer = MakeProcessRenderer({executable,
                                             root,
                                             {"--fixture", "success", "{output}", "{input}"},
                                             std::chrono::seconds(2),
                                             true});
  const auto result = renderer(test, 1, 1);
  if (!SameChildDisposition(before)) {
    return 12;
  }
  if (!disposition.restore()) {
    return 13;
  }
  return CheckPolicyFixtureOutcome(mode, result, root);
}

struct RecordingChildOperations {
  internal::ChildObservation observation = internal::ChildObservation::Observed;
  std::vector<pid_t> signals;
  bool reaped = false;
  void killProcessOrGroup(pid_t target) { signals.push_back(target); }
  internal::ChildObservation observeExit(pid_t) { return observation; }
  bool reapChild(pid_t, int&) {
    reaped = true;
    return true;
  }
};

TEST(ProcessCleanupTest, KnownOwnershipLossNeverSignalsOrReaps) {
  RecordingChildOperations operations;
  int status = 0;
  EXPECT_FALSE(internal::FinishChild(42, true, status, operations));
  EXPECT_TRUE(operations.signals.empty());
  EXPECT_FALSE(operations.reaped);
}

TEST(ProcessCleanupTest, LostOwnershipDuringObservationStopsFurtherSignals) {
  RecordingChildOperations operations;
  operations.observation = internal::ChildObservation::OwnershipLost;
  int status = 0;
  EXPECT_FALSE(internal::FinishChild(42, false, status, operations));
  EXPECT_THAT(operations.signals, testing::ElementsAre(-42, 42));
  EXPECT_FALSE(operations.reaped);
}

TEST(ProcessCleanupTest, FailedObservationOnlyAttemptsNonsignalingCollection) {
  RecordingChildOperations operations;
  operations.observation = internal::ChildObservation::Failed;
  int status = 0;
  EXPECT_FALSE(internal::FinishChild(42, false, status, operations));
  EXPECT_THAT(operations.signals, testing::ElementsAre(-42, 42));
  EXPECT_TRUE(operations.reaped);
}

TEST(ProcessCleanupTest, ReservedLeaderAllowsFinalGroupCleanupThenReaping) {
  RecordingChildOperations operations;
  int status = 0;
  EXPECT_TRUE(internal::FinishChild(42, false, status, operations));
  EXPECT_THAT(operations.signals, testing::ElementsAre(-42, 42, -42));
  EXPECT_TRUE(operations.reaped);
}

class ProcessRendererTest : public testing::Test {
protected:
  void SetUp() override {
    const char* temporary = std::getenv("TEST_TMPDIR");
    ASSERT_NE(temporary, nullptr) << "TEST_TMPDIR must name a test-owned work directory";
    std::string pattern = (std::filesystem::path(temporary) / "process-tests-XXXXXX").string();
    ASSERT_NE(mkdtemp(pattern.data()), nullptr);
    root_ = pattern;
    test_ = StaticCase{"fixture", root_, root_ / "input {root} {output} ; literal.svg",
                       root_ / "expected.png"};
  }

  ImageResult render(std::string mode,
                     std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    return MakeProcessRenderer(
        {executable, root_, {"--fixture", std::move(mode), "{output}", "{input}"}, timeout, true})(
        test_, 1, 1);
  }

  int runPolicyFixture(const char* mode) {
    const std::string executablePath = executable.string();
    const std::string directory = root_.string();
    const pid_t child = fork();
    if (child < 0) {
      return -1;
    }
    if (child == 0) {
      execl(executablePath.c_str(), executablePath.c_str(), "--reaping-policy-fixture", mode,
            directory.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    int status = 0;
    pid_t collected;
    do {
      collected = waitpid(child, &status, 0);
    } while (collected < 0 && errno == EINTR);
    return collected == child && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }

  std::filesystem::path root_;
  StaticCase test_;
};

TEST_F(ProcessRendererTest, EmbeddedNulArgumentIsRejectedBeforeLaunch) {
  const auto result = render(std::string("success\0unrecorded", 18));
  EXPECT_EQ(result.image, std::nullopt);
  EXPECT_THAT(result.error, testing::HasSubstr("UTF-8"));
  EXPECT_FALSE(std::filesystem::exists(root_ / "fixture/process/render.png"));
}

TEST_F(ProcessRendererTest, InvalidUtf8ArgumentIsRejectedBeforeLaunch) {
  const auto result = render(std::string("invalid-") + char(0xFF));
  EXPECT_EQ(result.image, std::nullopt);
  EXPECT_THAT(result.error, testing::HasSubstr("UTF-8"));
  EXPECT_FALSE(std::filesystem::exists(root_ / "fixture/process"));
}

TEST_F(ProcessRendererTest, ValidUnicodeArgumentsRemainExactInProcessEvidence) {
  test_.svgPath = root_ / "\xC3\xA9" / test_.svgPath.filename();
  const auto result = render("success");
  ASSERT_THAT(result.error, testing::IsEmpty());
  ASSERT_TRUE(result.image);
  std::ifstream input(root_ / "fixture/process/process.json");
  const std::string record((std::istreambuf_iterator<char>(input)), {});
  EXPECT_NE(record.find("\xC3\xA9"), std::string::npos);
}

TEST_F(ProcessRendererTest, OversizedDirectArgumentsAreRejectedBeforeLaunch) {
  const auto result = render(std::string(65537, 'x'));
  EXPECT_EQ(result.image, std::nullopt);
  EXPECT_THAT(result.error, testing::HasSubstr("bounded"));
  EXPECT_FALSE(std::filesystem::exists(root_ / "fixture/process"));
}

TEST_F(ProcessRendererTest, ExpandedArgumentsRemainBoundedBeforeLaunch) {
  test_.svgPath = root_ / std::string(400, 'x');
  std::string argument;
  for (int index = 0; index < 200; ++index) {
    argument += "{input}";
  }
  const auto result = render(argument);
  EXPECT_EQ(result.image, std::nullopt);
  EXPECT_THAT(result.error, testing::HasSubstr("bounded"));
  EXPECT_FALSE(std::filesystem::exists(root_ / "fixture/process/render.png"));
}

TEST_F(ProcessRendererTest, IgnoredChildSignalsRejectBeforeRendererLaunch) {
  EXPECT_EQ(runPolicyFixture("ignored"), 0);
}

TEST_F(ProcessRendererTest, AutoReapedChildrenRejectBeforeRendererLaunch) {
  EXPECT_EQ(runPolicyFixture("auto-reap"), 0);
}

TEST_F(ProcessRendererTest, NotificationOnlyChildHandlerRemainsSupported) {
  EXPECT_EQ(runPolicyFixture("notify"), 0);
}

TEST_F(ProcessRendererTest, RendererHasOwnGroupWithinParentSession) {
  const auto result = render("identity");
  EXPECT_THAT(result.error, testing::IsEmpty());
  EXPECT_TRUE(result.image);
}

TEST_F(ProcessRendererTest, SuccessfulRenderPreservesLiteralArguments) {
  const auto result = render("success");
  ASSERT_THAT(result.error, testing::IsEmpty());
  ASSERT_NE(result.image, std::nullopt);
  EXPECT_THAT(result.image->data, testing::ElementsAre(255, 0, 0, 255));
}

TEST_F(ProcessRendererTest, SuccessfulRendererDoesNotLeaveDescendantRunning) {
  DescendantProbe probe;
  ASSERT_TRUE(probe.open());
  const auto start = std::chrono::steady_clock::now();
  const auto result = render(probe.mode("success"));
  EXPECT_THAT(result.error, testing::IsEmpty());
  EXPECT_NE(result.image, std::nullopt);
  EXPECT_TRUE(probe.stopped()) << "successful renderer left a live descendant";
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
}

TEST_F(ProcessRendererTest, FailedRendererDoesNotLeaveDescendantRunning) {
  DescendantProbe probe;
  ASSERT_TRUE(probe.open());
  const auto result = render(probe.mode("failure"));
  EXPECT_THAT(result.error, testing::HasSubstr("renderer exit 7"));
  EXPECT_TRUE(probe.stopped()) << "failed renderer left a live descendant";
}

TEST_F(ProcessRendererTest, TimedOutRendererDoesNotLeaveDescendantRunning) {
  DescendantProbe probe;
  ASSERT_TRUE(probe.open());
  const auto result = render(probe.mode("timeout"), std::chrono::seconds(1));
  EXPECT_THAT(result.error, testing::HasSubstr("deadline exceeded"));
  EXPECT_TRUE(probe.stopped()) << "timed-out renderer left a live descendant";
}

TEST_F(ProcessRendererTest, NonExecutableRendererReportsExecStageAndErrno) {
  const auto blocked = root_ / "not-executable";
  std::ofstream(blocked) << "not an executable\n";
  std::error_code error;
  std::filesystem::permissions(
      blocked, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace, error);
  ASSERT_FALSE(error) << error;
  const auto renderer = MakeProcessRenderer({blocked, root_, {}, std::chrono::seconds(2), true});
  const auto result = renderer(test_, 1, 1);
  EXPECT_EQ(result.image, std::nullopt);
  EXPECT_THAT(result.error, testing::HasSubstr("renderer exit 127"));
  EXPECT_THAT(result.error,
              testing::HasSubstr("renderer setup stage=execv errno=" + std::to_string(EACCES)));
}

TEST_F(ProcessRendererTest, NonzeroExitIsAnError) {
  EXPECT_THAT(render("failure").error, testing::HasSubstr("renderer exit 7"));
}

TEST_F(ProcessRendererTest, MissingOutputCannotPass) {
  const auto result = render("missing");
  EXPECT_EQ(result.image, std::nullopt);
  EXPECT_THAT(result.error, testing::Not(testing::IsEmpty()));
}

TEST_F(ProcessRendererTest, DeadlineKillsAndCollectsChild) {
  const auto start = std::chrono::steady_clock::now();
  const auto result = render("timeout", std::chrono::milliseconds(50));
  EXPECT_THAT(result.error, testing::HasSubstr("deadline exceeded"));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
}

TEST_F(ProcessRendererTest, DiagnosticsAreBounded) {
  const auto result = render("diagnostics");
  EXPECT_THAT(result.error, testing::HasSubstr("renderer exit 7"));
  EXPECT_LE(result.error.size(), 16416u);
}

#if defined(__linux__)
TEST_F(ProcessRendererTest, AddressSpaceLimitRejectsAllocation) {
  EXPECT_THAT(render("memory").error, testing::HasSubstr("renderer exit 7"));
}
#else
TEST_F(ProcessRendererTest, UntrustedMemoryModeFailsClosed) {
  const auto renderer = MakeProcessRenderer({executable, root_, {}, std::chrono::seconds(1)});
  EXPECT_THAT(renderer(test_, 1, 1).error, testing::HasSubstr("memory limits are unavailable"));
}
#endif

TEST_F(ProcessRendererTest, InvalidDeadlineIsRejected) {
  EXPECT_THAT(render("success", std::chrono::milliseconds(0)).error,
              testing::HasSubstr("timeout must be"));
}

TEST_F(ProcessRendererTest, SymlinkOutputCannotEscape) {
  EXPECT_THAT(render("symlink").error, testing::HasSubstr("not a symlink"));
}

}  // namespace
}  // namespace donner::svg_suite

namespace {

int writeFixtureImage(const char* output, const char* input) {
  if (getpgrp() != getpid()) {
    return 15;
  }
  if (!std::string_view(input).ends_with("input {root} {output} ; literal.svg")) {
    return 9;
  }
  const std::vector<uint8_t> pixels{255, 0, 0, 255};
  return donner::svg_suite::WritePng(output, {1, 1, 1, pixels}).empty() ? 0 : 10;
}

int descendantFixture(std::string_view mode, const char* output, const char* input) {
  const auto separator = mode.find(':');
  const int control = std::stoi(std::string(mode.substr(separator + 1)));
  const pid_t child = fork();
  if (child < 0) {
    return 13;
  }
  if (child == 0) {
    for (;;) {
      pause();
    }
  }
  if (write(control, &child, sizeof(child)) != static_cast<ssize_t>(sizeof(child))) {
    kill(child, SIGKILL);
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    return 14;
  }
  close(control);
  if (mode.starts_with("descendant-timeout:")) {
    for (;;) {
      pause();
    }
  }
  if (mode.starts_with("descendant-failure:")) {
    return 7;
  }
  return writeFixtureImage(output, input);
}

int runFixture(std::string_view mode, const char* output, const char* input) {
  if (mode == "identity") {
    if (getpgrp() != getpid() || getsid(0) != getsid(getppid())) {
      return 15;
    }
    return writeFixtureImage(output, input);
  }
  if (mode.starts_with("descendant-")) {
    return descendantFixture(mode, output, input);
  }
  if (mode == "memory") {
    void* memory = std::malloc(static_cast<size_t>(2) * 1024u * 1024u * 1024u);
    if (memory != nullptr) {
      std::free(memory);
      return 12;
    }
    return 7;
  }
  if (mode == "failure") {
    return 7;
  }
  if (mode == "missing") {
    return 0;
  }
  if (mode == "timeout") {
    for (;;) {
      poll(nullptr, 0, 1000);
    }
  }
  if (mode == "diagnostics") {
    for (int i = 0; i < 100; ++i) {
      std::cerr << std::string(1024, 'x');
    }
    return 7;
  }
  if (mode == "symlink") {
    std::error_code error;
    std::filesystem::create_symlink("/dev/null", output, error);
    return error ? 8 : 0;
  }
  if (mode == "success") {
    return writeFixtureImage(output, input);
  }
  return 11;
}

}  // namespace

int main(int argc, char** argv) {
  donner::svg_suite::executable = std::filesystem::canonical(argv[0]);
  if (argc == 4 && std::string_view(argv[1]) == "--reaping-policy-fixture") {
    return donner::svg_suite::ReapingPolicyFixture(argv[2], argv[3]);
  }
  if (argc == 5 && std::string_view(argv[1]) == "--fixture") {
    return runFixture(argv[2], argv[3], argv[4]);
  }
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
