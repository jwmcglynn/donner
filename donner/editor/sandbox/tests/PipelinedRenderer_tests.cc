/// @file
///
/// Tests for `PipelinedRenderer` — the in-process, multi-threaded variant of
/// the sandbox wire pipeline. Verifies:
///  1. `submit()` → `waitForFrame()` produces a bitmap equal to an in-process
///     render for the same SVG (lossless across the thread boundary).
///  2. Multiple submissions each produce a distinct `frameId` and return the
///     newest rasterized frame.
///  3. "Newest wins" — queueing several frames without draining only requires
///     the last one to survive.
///  4. The worker thread shuts down cleanly when the renderer is destroyed,
///     even if a frame is still in-flight.

#include <gtest/gtest.h>

#include <chrono>
#include <string_view>
#include <thread>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/sandbox/PipelinedRenderer.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::editor::sandbox {
namespace {

svg::SVGDocument ParseOrDie(std::string_view svg) {
  ParseWarningSink warnings;
  auto result = svg::parser::SVGParser::ParseSVG(svg, warnings);
  EXPECT_FALSE(result.hasError()) << result.error();
  return std::move(result.result());
}

svg::RendererBitmap RenderInProcess(std::string_view svg, int w, int h) {
  auto doc = ParseOrDie(svg);
  doc.setCanvasSize(w, h);
  svg::Renderer renderer;
  renderer.draw(doc);
  return renderer.takeSnapshot();
}

constexpr std::string_view kSimpleSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
       <rect width="32" height="32" fill="red"/>
     </svg>)";

TEST(PipelinedRendererTest, SingleFrameMatchesInProcess) {
  PipelinedRenderer pipeline;
  auto doc = ParseOrDie(kSimpleSvg);

  const uint64_t id = pipeline.submit(doc, 32, 32);
  const auto frame = pipeline.waitForFrame(id);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->frameId, id);
  EXPECT_TRUE(frame->ok);
  EXPECT_EQ(frame->unsupportedCount, 0u);

  const auto direct = RenderInProcess(kSimpleSvg, 32, 32);
  ASSERT_EQ(frame->bitmap.dimensions, direct.dimensions);
  EXPECT_EQ(frame->bitmap.pixels, direct.pixels)
      << "pipeline rasterization must be pixel-identical to in-process";
}

TEST(PipelinedRendererTest, FrameIdsMonotonicallyIncrease) {
  PipelinedRenderer pipeline;
  auto doc1 = ParseOrDie(kSimpleSvg);
  auto doc2 = ParseOrDie(kSimpleSvg);
  auto doc3 = ParseOrDie(kSimpleSvg);

  const uint64_t id1 = pipeline.submit(doc1, 32, 32);
  const uint64_t id2 = pipeline.submit(doc2, 32, 32);
  const uint64_t id3 = pipeline.submit(doc3, 32, 32);
  EXPECT_LT(id1, id2);
  EXPECT_LT(id2, id3);

  // Wait for the highest frame id — the worker will eventually complete
  // something ≥ id3, possibly after skipping id1/id2 under the newest-wins
  // policy.
  const auto frame = pipeline.waitForFrame(id3);
  ASSERT_TRUE(frame.has_value());
  EXPECT_GE(frame->frameId, id3);
}

TEST(PipelinedRendererTest, NewestWinsWhenQueuingSeveralFrames) {
  PipelinedRenderer pipeline;

  // Fire several submits rapidly; only the last one is guaranteed to
  // eventually land. The worker may happen to rasterize earlier ones, but
  // nothing in the API promises that.
  uint64_t lastId = 0;
  for (int i = 0; i < 5; ++i) {
    auto doc = ParseOrDie(kSimpleSvg);
    lastId = pipeline.submit(doc, 32, 32);
  }

  const auto frame = pipeline.waitForFrame(lastId);
  ASSERT_TRUE(frame.has_value());
  EXPECT_GE(frame->frameId, lastId);
  EXPECT_TRUE(frame->ok);
}

TEST(PipelinedRendererTest, ShutdownIsClean) {
  // Smoke test: constructing and destructing a pipeline without any frames
  // must not deadlock. Also: destructing mid-submission must not crash.
  {
    PipelinedRenderer idle;
  }
  {
    PipelinedRenderer inFlight;
    auto doc = ParseOrDie(kSimpleSvg);
    inFlight.submit(doc, 32, 32);
    // Do not wait — the destructor should join the worker regardless.
  }
}

TEST(PipelinedRendererTest, AcquireLatestReturnsNulloptBeforeFirstFrame) {
  PipelinedRenderer pipeline;
  EXPECT_FALSE(pipeline.acquireLatestFrame().has_value());
}

TEST(PipelinedRendererTest, PipelineMatchesComplexRender) {
  constexpr std::string_view kComplex =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <g transform="translate(10 10)">
           <rect width="30" height="30" fill="green"/>
           <circle cx="60" cy="60" r="20" fill="orange"/>
         </g>
       </svg>)svg";

  PipelinedRenderer pipeline;
  auto doc = ParseOrDie(kComplex);
  const uint64_t id = pipeline.submit(doc, 100, 100);
  const auto frame = pipeline.waitForFrame(id);
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->ok);

  const auto direct = RenderInProcess(kComplex, 100, 100);
  ASSERT_EQ(frame->bitmap.dimensions, direct.dimensions);
  EXPECT_EQ(frame->bitmap.pixels, direct.pixels);
}

}  // namespace
}  // namespace donner::editor::sandbox
