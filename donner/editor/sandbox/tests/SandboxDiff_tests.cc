/// @file
///
/// Tests for `ComputeRnrDiff` — the structural diff engine for `.rnr`
/// recordings. All wire fixtures are built programmatically via
/// `SerializingRenderer::draw()` and `EncodeRnrBuffer` / `ParseRnrBuffer`.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/sandbox/FrameInspector.h"
#include "donner/editor/sandbox/RnrFile.h"
#include "donner/editor/sandbox/SandboxDiff.h"
#include "donner/editor/sandbox/SerializingRenderer.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor::sandbox {
namespace {

constexpr std::string_view kSvgA =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
       <rect width="64" height="64" fill="red"/>
     </svg>)";

constexpr std::string_view kSvgB =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
       <rect width="64" height="64" fill="blue"/>
     </svg>)";

constexpr std::string_view kSvgExtra =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
       <rect width="64" height="64" fill="red"/>
       <rect x="10" y="10" width="20" height="20" fill="green"/>
     </svg>)";

svg::SVGDocument ParseOrDie(std::string_view svg) {
  ParseWarningSink warnings;
  auto result = svg::parser::SVGParser::ParseSVG(svg, warnings);
  EXPECT_FALSE(result.hasError()) << result.error();
  return std::move(result.result());
}

std::vector<uint8_t> SerializeFrame(std::string_view svg, int w, int h) {
  auto doc = ParseOrDie(svg);
  doc.setCanvasSize(w, h);
  SerializingRenderer serializer;
  serializer.draw(doc);
  return std::move(serializer).takeBuffer();
}

RnrHeader MakeHeader(const std::string& uri = "memory://test") {
  RnrHeader header;
  header.width = 64;
  header.height = 64;
  header.backend = BackendHint::kTinySkia;
  header.uri = uri;
  return header;
}

// --------------------------------------------------------------------------
// Tests
// --------------------------------------------------------------------------

TEST(SandboxDiffTest, IdenticalStreamsReportNoDiff) {
  const auto wire = SerializeFrame(kSvgA, 64, 64);
  const auto header = MakeHeader();
  const auto decoded = FrameInspector::Decode(wire);
  ASSERT_TRUE(decoded.streamValid) << decoded.error;

  const auto diff = ComputeRnrDiff(header, decoded.commands, header, decoded.commands);
  EXPECT_TRUE(diff.identical);
  EXPECT_TRUE(diff.report.empty());
}

TEST(SandboxDiffTest, DifferentDrawCallReported) {
  const auto wireA = SerializeFrame(kSvgA, 64, 64);
  const auto wireB = SerializeFrame(kSvgB, 64, 64);
  const auto header = MakeHeader();

  const auto decodedA = FrameInspector::Decode(wireA);
  const auto decodedB = FrameInspector::Decode(wireB);
  ASSERT_TRUE(decodedA.streamValid) << decodedA.error;
  ASSERT_TRUE(decodedB.streamValid) << decodedB.error;

  const auto diff = ComputeRnrDiff(header, decodedA.commands, header, decodedB.commands);
  EXPECT_FALSE(diff.identical);
  // The diff should contain at least one '-' and one '+' line.
  EXPECT_NE(diff.report.find('-'), std::string::npos);
  EXPECT_NE(diff.report.find('+'), std::string::npos);
}

TEST(SandboxDiffTest, HeaderMismatchReported) {
  const auto wire = SerializeFrame(kSvgA, 64, 64);
  const auto decoded = FrameInspector::Decode(wire);
  ASSERT_TRUE(decoded.streamValid) << decoded.error;

  auto headerA = MakeHeader("file:///a.svg");
  auto headerB = MakeHeader("file:///b.svg");

  const auto diff = ComputeRnrDiff(headerA, decoded.commands, headerB, decoded.commands);
  EXPECT_FALSE(diff.identical);
  EXPECT_NE(diff.report.find("uri:"), std::string::npos);
}

TEST(SandboxDiffTest, InsertedCommandShowsUpWithPlus) {
  const auto wireA = SerializeFrame(kSvgA, 64, 64);
  const auto wireB = SerializeFrame(kSvgExtra, 64, 64);
  const auto header = MakeHeader();

  const auto decodedA = FrameInspector::Decode(wireA);
  const auto decodedB = FrameInspector::Decode(wireB);
  ASSERT_TRUE(decodedA.streamValid) << decodedA.error;
  ASSERT_TRUE(decodedB.streamValid) << decodedB.error;

  // B has more commands than A.
  EXPECT_GT(decodedB.commands.size(), decodedA.commands.size());

  const auto diff = ComputeRnrDiff(header, decodedA.commands, header, decodedB.commands);
  EXPECT_FALSE(diff.identical);
  EXPECT_NE(diff.report.find('+'), std::string::npos);
}

}  // namespace
}  // namespace donner::editor::sandbox
