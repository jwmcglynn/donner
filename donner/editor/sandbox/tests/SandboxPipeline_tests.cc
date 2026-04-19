/// @file
///
/// End-to-end pipeline tests for the editor sandbox: spawn the child binary,
/// stream the wire format through a pipe, replay into a host-side backend
/// (via `Renderer`), and assert the resulting bitmap is pixel-identical to
/// an in-process render.
///
/// These tests close the loop on S1 + S2 + S3: process isolation (S1), wire
/// format (S2), and the host-side `renderToBackend` API (S3). A failure here
/// means something in that chain is lossy.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/editor/sandbox/SandboxHost.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::editor::sandbox {
namespace {

class SandboxPipelineTest : public ::testing::Test {
protected:
  SandboxHost MakeHost() {
    const std::string childPath =
        Runfiles::instance().Rlocation("donner/editor/sandbox/donner_parser_child");
    return SandboxHost(childPath);
  }

  static svg::RendererBitmap RenderInProcess(std::string_view svg, int w, int h) {
    ParseWarningSink warnings;
    auto parseResult = svg::parser::SVGParser::ParseSVG(svg, warnings);
    EXPECT_FALSE(parseResult.hasError()) << parseResult.error();
    svg::SVGDocument document = std::move(parseResult.result());
    document.setCanvasSize(w, h);
    svg::Renderer renderer;
    renderer.draw(document);
    return renderer.takeSnapshot();
  }
};

TEST_F(SandboxPipelineTest, SolidRectPixelIdenticalThroughSandbox) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
         <rect width="32" height="32" fill="red"/>
       </svg>)";

  const auto direct = RenderInProcess(kSvg, 32, 32);

  svg::Renderer viaBackend;
  auto host = MakeHost();
  const auto result = host.renderToBackend(kSvg, 32, 32, viaBackend);
  ASSERT_EQ(result.status, SandboxStatus::kOk) << "diagnostics: " << result.diagnostics;
  EXPECT_EQ(result.unsupportedCount, 0u);

  const auto viaWire = viaBackend.takeSnapshot();
  ASSERT_EQ(direct.dimensions, viaWire.dimensions);
  EXPECT_EQ(direct.pixels, viaWire.pixels)
      << "the sandbox child + wire pipeline must be pixel-identical to in-process";
}

TEST_F(SandboxPipelineTest, PathFillPixelIdenticalThroughSandbox) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
         <path d="M8 8 L56 8 L32 56 Z" fill="blue"/>
       </svg>)";

  const auto direct = RenderInProcess(kSvg, 64, 64);

  svg::Renderer viaBackend;
  auto host = MakeHost();
  const auto result = host.renderToBackend(kSvg, 64, 64, viaBackend);
  ASSERT_EQ(result.status, SandboxStatus::kOk) << result.diagnostics;
  EXPECT_EQ(result.unsupportedCount, 0u);

  const auto viaWire = viaBackend.takeSnapshot();
  ASSERT_EQ(direct.dimensions, viaWire.dimensions);
  EXPECT_EQ(direct.pixels, viaWire.pixels);
}

TEST_F(SandboxPipelineTest, TransformedGroupPixelIdenticalThroughSandbox) {
  constexpr std::string_view kSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <g transform="translate(10 10)">
           <rect width="30" height="30" fill="green"/>
           <circle cx="60" cy="60" r="20" fill="orange"/>
         </g>
       </svg>)svg";

  const auto direct = RenderInProcess(kSvg, 100, 100);

  svg::Renderer viaBackend;
  auto host = MakeHost();
  const auto result = host.renderToBackend(kSvg, 100, 100, viaBackend);
  ASSERT_EQ(result.status, SandboxStatus::kOk) << result.diagnostics;
  EXPECT_EQ(result.unsupportedCount, 0u);

  const auto viaWire = viaBackend.takeSnapshot();
  ASSERT_EQ(direct.dimensions, viaWire.dimensions);
  EXPECT_EQ(direct.pixels, viaWire.pixels);
}

TEST_F(SandboxPipelineTest, WireBytesAreCapturedOnSuccess) {
  constexpr std::string_view kSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10">
         <rect width="10" height="10" fill="black"/>
       </svg>)";

  svg::Renderer viaBackend;
  auto host = MakeHost();
  const auto result = host.renderToBackend(kSvg, 10, 10, viaBackend);
  ASSERT_EQ(result.status, SandboxStatus::kOk) << result.diagnostics;
  // The wire field must contain at least a stream header (8-byte payload +
  // 8-byte message header = 16 bytes) plus a begin/end frame pair.
  EXPECT_GE(result.wire.size(), 24u);
}

TEST_F(SandboxPipelineTest, MalformedSvgSurfacesAsParseError) {
  svg::Renderer viaBackend;
  auto host = MakeHost();
  const auto result = host.renderToBackend("definitely not svg", 10, 10, viaBackend);
  EXPECT_EQ(result.status, SandboxStatus::kParseError);
  EXPECT_TRUE(result.wire.empty());
  EXPECT_FALSE(result.diagnostics.empty());
}

}  // namespace
}  // namespace donner::editor::sandbox
