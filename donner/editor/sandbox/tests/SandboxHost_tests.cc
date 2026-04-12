/// @file
///
/// End-to-end tests for Milestone S1 of the editor sandbox: spawn the child
/// binary, render a few representative SVGs (valid, malformed, crashy), assert
/// host liveness and exit classification.
///
/// The core invariant these tests defend is simple: **no adversarial input can
/// crash the host process**. Every failure mode reported by `SandboxHost`
/// should be a normal `RenderResult`, never an abort or signal propagated to
/// the test runner.

#include "donner/editor/sandbox/SandboxHost.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "donner/base/tests/Runfiles.h"

namespace donner::editor::sandbox {
namespace {

using ::testing::Test;

class SandboxHostTest : public Test {
 protected:
  SandboxHost MakeHost() {
    const std::string childPath =
        Runfiles::instance().Rlocation("donner/editor/sandbox/donner_parser_child");
    return SandboxHost(childPath);
  }
};

constexpr std::string_view kTrivialSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="50" height="50">)"
    R"(<rect width="50" height="50" fill="red"/>)"
    R"(</svg>)";

constexpr std::array<uint8_t, 8> kPngMagic = {0x89, 0x50, 0x4E, 0x47,
                                              0x0D, 0x0A, 0x1A, 0x0A};

bool StartsWithPngMagic(const std::vector<uint8_t>& bytes) {
  if (bytes.size() < kPngMagic.size()) return false;
  return std::equal(kPngMagic.begin(), kPngMagic.end(), bytes.begin());
}

TEST_F(SandboxHostTest, RendersTrivialSvgToPng) {
  SandboxHost host = MakeHost();
  RenderResult result = host.render(kTrivialSvg, 50, 50);

  EXPECT_EQ(result.status, SandboxStatus::kOk) << "diagnostics: " << result.diagnostics;
  EXPECT_EQ(result.exitCode, 0);
  EXPECT_TRUE(StartsWithPngMagic(result.png)) << "png size=" << result.png.size();
  EXPECT_GT(result.png.size(), kPngMagic.size());
}

TEST_F(SandboxHostTest, ClassifiesParseErrorWithoutCrashingHost) {
  SandboxHost host = MakeHost();
  RenderResult result = host.render("this is not svg at all", 50, 50);

  EXPECT_EQ(result.status, SandboxStatus::kParseError);
  EXPECT_TRUE(result.png.empty());
  EXPECT_FALSE(result.diagnostics.empty())
      << "expected a stderr diagnostic from the child";
}

TEST_F(SandboxHostTest, EmptyInputIsParseError) {
  SandboxHost host = MakeHost();
  RenderResult result = host.render("", 50, 50);

  EXPECT_EQ(result.status, SandboxStatus::kParseError);
  EXPECT_TRUE(result.png.empty());
}

TEST_F(SandboxHostTest, LargeSvgDoesNotDeadlockPipes) {
  // Build an SVG larger than the default 64 KiB pipe buffer so that the host's
  // background-writer thread actually has to overlap with the child's reads.
  // If the SandboxHost pipe plumbing is wrong this test wedges forever.
  std::string svg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">)";
  svg.reserve(200 * 1024);
  for (int i = 0; i < 2000; ++i) {
    svg += R"(<rect x="1" y="1" width="2" height="2" fill="#010101"/>)";
  }
  svg += "</svg>";
  ASSERT_GT(svg.size(), 64u * 1024) << "test SVG should exceed a single pipe buffer";

  SandboxHost host = MakeHost();
  RenderResult result = host.render(svg, 100, 100);

  EXPECT_EQ(result.status, SandboxStatus::kOk) << "diagnostics: " << result.diagnostics;
  EXPECT_TRUE(StartsWithPngMagic(result.png));
}

TEST_F(SandboxHostTest, RejectsInvalidDimensions) {
  SandboxHost host = MakeHost();
  RenderResult result = host.render(kTrivialSvg, /*width=*/0, /*height=*/50);

  EXPECT_EQ(result.status, SandboxStatus::kUsageError);
  EXPECT_TRUE(result.png.empty());
}

// Regression guard for the primary threat model: the host must survive an
// arbitrary byte-stream that would otherwise crash the parser. We can't
// easily fabricate a parser crash on demand (SVGParser is fuzzed and
// noexcept), so this test checks the next-best thing: whatever the worst case
// is, `RenderResult` comes back cleanly rather than aborting the test runner.
TEST_F(SandboxHostTest, AdversarialBytesNeverCrashTheHost) {
  SandboxHost host = MakeHost();

  // A mix of partial tags, enormous attribute values, and binary garbage.
  std::string payload = "<svg xmlns='http://www.w3.org/2000/svg'><";
  payload.append(100, '\xff');
  payload += "path d='";
  payload.append(50'000, 'M');
  payload += "'/></svg>";

  RenderResult result = host.render(payload, 100, 100);
  // We don't care whether this is kOk or kParseError — we care that we got a
  // RenderResult at all. Failing this test means the host crashed.
  EXPECT_TRUE(result.status == SandboxStatus::kOk ||
              result.status == SandboxStatus::kParseError ||
              result.status == SandboxStatus::kRenderError ||
              result.status == SandboxStatus::kCrashed)
      << "unexpected status " << static_cast<int>(result.status);
}

}  // namespace
}  // namespace donner::editor::sandbox
