/// @file
/// How many synchronous browser GPU owner round trips the compositor makes per held drag frame of
/// a masked child, counted over fake browser bridges. In the browser editor every one of them
/// blocks the raster worker until the canvas-owning thread runs it, so this count multiplies the
/// owner's per-call latency into every held result's render time.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Transform.h"
#include "donner/base/tests/RunfileGate.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/geode/tests/FakeBrowserGeodeContext.h"

namespace donner::editor {
namespace {

using geode::tests::BrowserOwnerRoundTrips;
using geode::tests::FakeBrowserGeodeContext;
using svg::compositor::CompositorController;
using svg::compositor::InteractionHint;

/// One held frame of the splash document's masked crown face drag.
struct HeldFrame {
  double x = 0.0;                     //!< Crown face translation the frame renders.
  BrowserOwnerRoundTrips roundTrips;  //!< Round trips the frame makes today.
};

/// The recorded baseline. A change that lowers these updates them; one that raises them must
/// explain why.
constexpr HeldFrame kHeldFrames[] = {
    {12.0,
     {.ownershipQuery = 95,
      .completionQuery = 86,
      .resourceCreation = 45,
      .resourceDestruction = 72,
      .bufferWrite = 44,
      .resourceSharing = 2,
      .commandBatch = 5,
      .queueSubmit = 3,
      .bindGroupConstruction = 45}},
    {24.0,
     {.ownershipQuery = 74,
      .completionQuery = 82,
      .resourceCreation = 36,
      .resourceDestruction = 68,
      .bufferWrite = 53,
      .resourceSharing = 2,
      .commandBatch = 5,
      .queueSubmit = 3,
      .bindGroupConstruction = 33}},
    {36.0,
     {.ownershipQuery = 70,
      .completionQuery = 78,
      .resourceCreation = 35,
      .resourceDestruction = 64,
      .bufferWrite = 55,
      .resourceSharing = 2,
      .commandBatch = 5,
      .queueSubmit = 3,
      .bindGroupConstruction = 30}},
};

TEST(BrowserOwnerRoundTripTest, MaskedChildHeldDragFrameMatchesTheRecordedBaseline) {
  FakeBrowserGeodeContext context = FakeBrowserGeodeContext::Open();
  ASSERT_NE(context.device, nullptr) << "a Geode context over fake browser bridges must open";
  const donner::tests::RequiredRunfile source =
      donner::tests::ReadRequiredRunfile("geode_splash.svg");
  DONNER_REQUIRE_RUNFILE(source);
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = svg::parser::SVGParser::ParseSVG(source.contents, warnings);
  ASSERT_FALSE(parsed.hasError());
  svg::SVGDocument document = std::move(parsed.result());
  svg::RenderViewport viewport;
  viewport.size = Vector2d(1536, 1024);
  viewport.devicePixelRatio = 1.0;
  document.setCanvasSize(1536, 1024);
  const std::optional<svg::SVGElement> target = document.querySelector("#central-crown-face-3");
  ASSERT_TRUE(target.has_value());

  svg::Renderer renderer(context.device);
  svg::compositor::CompositorConfig config;
  config.deferFirstFrameWarmup = false;
  CompositorController compositor(document, renderer, config);
  compositor.renderFrame(viewport, Transform2d());
  ASSERT_EQ(
      compositor.promoteEntity(target->unsafeEntityHandle().entity(), InteractionHint::ActiveDrag),
      CompositorController::PromoteResult::OwningTilesRequired)
      << "the crown face stays inside its masked owner";
  // The drag start, which finds nothing dirty.
  compositor.renderFrame(viewport, Transform2d());

  for (const HeldFrame& frame : kHeldFrames) {
    SCOPED_TRACE(testing::Message() << "held move to x=" << frame.x);
    target->cast<svg::SVGGraphicsElement>().setTransform(
        Transform2d::Translate(Vector2d(frame.x, 0)));
    const CompositorController::FastPathCounters before = compositor.fastPathCountersForTesting();
    context.takeRoundTrips();
    compositor.renderFrame(viewport, Transform2d());
    const BrowserOwnerRoundTrips roundTrips = context.takeRoundTrips();
    const CompositorController::FastPathCounters after = compositor.fastPathCountersForTesting();
    ASSERT_EQ(after.fastPathFrames - before.fastPathFrames, 1u)
        << "the held move must be a fast-path frame for its round trips to be the fast path's";
    ASSERT_FALSE(context.device->isDeviceLost());
    EXPECT_EQ(roundTrips, frame.roundTrips);
  }
}

}  // namespace
}  // namespace donner::editor
