/// @file
/// How many synchronous browser GPU owner round trips the compositor makes per held drag frame of
/// a masked child, counted over fake browser bridges. In the browser editor every one of them
/// blocks the raster worker until the canvas-owning thread runs it, so this count multiplies the
/// owner's per-call latency into every held result's render time.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cctype>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <string>

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
  /// Deferred calls held after the frame's last round trip, sent by the frame-end flush.
  std::size_t trailingDeferredCalls = 0;
};

/// The recorded baseline. A change that lowers these updates them; one that raises them must
/// explain why. Ownership, loss and completion queries are absent: the browser side publishes
/// that state, so a held frame reads it without a round trip. Buffer writes, bind group
/// construction and destruction are absent too: they ride along with the next round trip, and the
/// 19 still held after a frame's last round trip go in one more batch when the worker parks.
constexpr HeldFrame kHeldFrames[] = {
    // The first held frame creates one pooled uniform arena buffer, taken by the layer tile draws
    // (`RendererGeode::drawTextureSnapshot`), that earlier frames no longer warm: mask composites
    // keep their own uniform buffers instead of drawing through the arena. Later held frames create
    // none beyond the recorded counts.
    {12.0, {.resourceCreation = 46, .resourceSharing = 2, .commandBatch = 6, .queueSubmit = 3}, 19},
    {24.0, {.resourceCreation = 36, .resourceSharing = 2, .commandBatch = 6, .queueSubmit = 3}, 19},
    {36.0, {.resourceCreation = 35, .resourceSharing = 2, .commandBatch = 6, .queueSubmit = 3}, 19},
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
    // Calls still held after the frame's last round trip go in one round trip of their own when
    // the worker parks, rather than waiting for the next frame.
    std::cout << "held move x=" << frame.x
              << " trailing deferred calls=" << context.trailingDeferredCalls() << "\n";
    EXPECT_EQ(context.trailingDeferredCalls(), frame.trailingDeferredCalls);
    EXPECT_EQ(roundTrips, frame.roundTrips);
  }
}

/// How the browser bridge dispatches one of its methods, read from its source.
struct BridgeDispatch {
  std::string kind = "Other";  //!< `BrowserGpuOperationKind` its round trip counts as.
  /// The `BrowserDeferredOperation` it defers, or empty for none.
  std::string deferredAs;
  bool recorded = false;  //!< Whether it records into, or sends, the command batch.
};

/// Each member function of the browser bridge, of any return type, and how its body dispatches.
std::map<std::string, BridgeDispatch> ReadBridgeDispatch(const std::string& source) {
  std::map<std::string, BridgeDispatch> methods;
  const std::regex method(R"(\n[^\n(]* ?EmscriptenBrowserBridge::(\w+)\()");
  const std::regex kind(R"(BrowserGpuOperationKind::(\w+))");
  const std::regex deferredAs(R"(deferCall\(\s*BrowserDeferredOperation::(\w+))");
  for (std::sregex_iterator it(source.begin(), source.end(), method), end; it != end; ++it) {
    const std::size_t bodyStart = static_cast<std::size_t>(it->position(0));
    const std::size_t bodyEnd = source.find("\n}\n", bodyStart);
    const std::string body = source.substr(bodyStart, bodyEnd - bodyStart);
    BridgeDispatch dispatch;
    if (std::smatch match; std::regex_search(body, match, deferredAs)) {
      dispatch.deferredAs = match[1];
    }
    if (std::smatch match; std::regex_search(body, match, kind)) {
      dispatch.kind = match[1];
    }
    // A command batch is how a recording travels, whether a method queues into it or sends it.
    dispatch.recorded =
        body.find("queueCommand(") != std::string::npos || dispatch.kind == "CommandBatch";
    methods[(*it)[1]] = dispatch;
  }
  return methods;
}

// The round-trip count above models the bridge's dispatch by hand. This holds the model to the
// bridge: every modeled operation dispatches as modeled, and every other operation outside
// command recording is a direct round trip of kind Other, which is what the model counts it as.
TEST(BrowserOwnerRoundTripTest, CountModelMatchesTheBrowserBridgesDispatch) {
  const donner::tests::RequiredRunfile source =
      donner::tests::ReadRequiredRunfile("donner/gpu/browser/EmscriptenBrowserBridge.cc");
  DONNER_REQUIRE_RUNFILE(source);
  std::map<std::string, BridgeDispatch> methods = ReadBridgeDispatch(source.contents);
  ASSERT_GT(methods.size(), 30u) << "the bridge source no longer reads as expected";

  // The bridge's own list of deferred operations names the method that defers each, and the
  // count reads deferral from that list, so the two cannot disagree on which calls are held.
  methods.erase("deferCall");
  for (const geode::tests::BrowserOperationModel& model : geode::tests::BrowserOperationModels()) {
    SCOPED_TRACE(std::string(model.operation));
    const auto found = methods.find(std::string(model.operation));
    ASSERT_NE(found, methods.end()) << "the bridge has no such method";
    if (model.deferred) {
      std::string enumerator(model.operation);
      enumerator[0] = static_cast<char>(std::toupper(enumerator[0]));
      EXPECT_EQ(found->second.deferredAs, enumerator) << "defers as its own operation";
    } else {
      EXPECT_EQ(found->second.deferredAs, "");
      EXPECT_EQ(found->second.kind, model.kind);
    }
    methods.erase(found);
  }
  for (const auto& [name, dispatch] : methods) {
    SCOPED_TRACE(name);
    EXPECT_EQ(dispatch.deferredAs, "") << "a deferred operation the count does not model";
    if (dispatch.recorded) {
      continue;
    }
    // Query kinds are counted from the fake bridge's own query counts, not from recorded lines;
    // starting publication is one such query, made once when a device is created.
    if (dispatch.kind == "OwnershipQuery" || dispatch.kind == "DeviceLossQuery" ||
        dispatch.kind == "CompletionQuery") {
      continue;
    }
    EXPECT_EQ(dispatch.kind, "Other") << "a direct operation the count would misfile as Other";
  }
}

}  // namespace
}  // namespace donner::editor
