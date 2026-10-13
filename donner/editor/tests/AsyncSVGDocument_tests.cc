#include "donner/editor/AsyncSVGDocument.h"

#include <gmock/gmock.h>

#include <array>
#include <future>
#include <limits>
#include <thread>

#include "donner/svg/DonnerController.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RenderingContext.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

using ::donner::svg::test::Rgba;
using ::testing::DoubleNear;
using ::testing::Field;
using ::testing::Gt;
using ::testing::Lt;

constexpr std::string_view kTrivialSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="0" y="0" width="10" height="10" fill="red"/>
       </svg>)";

constexpr std::string_view kReplacementSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r2" x="50" y="50" width="20" height="20" fill="blue"/>
       </svg>)";

std::array<std::uint8_t, 4> PixelAt(const svg::RendererBitmap& bitmap, int x, int y) {
  const std::size_t offset =
      static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
  return {
      bitmap.pixels[offset + 0u],
      bitmap.pixels[offset + 1u],
      bitmap.pixels[offset + 2u],
      bitmap.pixels[offset + 3u],
  };
}

svg::RendererBitmap RenderDocument(svg::SVGDocument& document) {
  svg::Renderer renderer;
  renderer.draw(document);
  return renderer.takeSnapshot();
}

MATCHER_P2(Vector2dNear, expected, tolerance, "") {
  return testing::ExplainMatchResult(
      testing::AllOf(Field("x", &Vector2d::x, DoubleNear(expected.x, tolerance)),
                     Field("y", &Vector2d::y, DoubleNear(expected.y, tolerance))),
      arg, result_listener);
}

TEST(AsyncSVGDocumentTest, EmptyByDefault) {
  AsyncSVGDocument doc;
  EXPECT_FALSE(doc.hasDocument());
  EXPECT_EQ(doc.currentFrameVersion(), 0u);
  EXPECT_FALSE(doc.flushFrame());
  EXPECT_EQ(doc.currentFrameVersion(), 0u);
  EXPECT_FALSE(doc.lastFlushResult().appliedCommands);
  EXPECT_TRUE(doc.parseDiagnostics().empty());
  EXPECT_EQ(doc.parseDiagnosticsRevision(), 0u);
}

TEST(AsyncSVGDocumentTest, LoadFromStringSucceedsAndBumpsVersion) {
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));
  EXPECT_TRUE(doc.hasDocument());
  EXPECT_EQ(doc.currentFrameVersion(), 1u);

  auto rect = doc.document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
}

TEST(AsyncSVGDocumentTest, AnimationClockPreservesSourceIdentityAndCommands) {
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));
  const std::string source(doc.document().source());
  const auto handle = doc.document().handle();
  const std::uint64_t generation = doc.documentGeneration();
  const std::uint64_t version = doc.currentFrameVersion();

  EXPECT_TRUE(doc.setAnimationTime(0.5));
  EXPECT_EQ(doc.document().currentTime(), 0.5);
  EXPECT_EQ(doc.currentFrameVersion(), version + 1);
  EXPECT_EQ(doc.documentGeneration(), generation);
  EXPECT_EQ(doc.document().handle(), handle);
  EXPECT_EQ(doc.document().source(), source);
  EXPECT_FALSE(doc.hasPendingMutations());
  EXPECT_FALSE(doc.hasUnmirroredSourceDeltas());
  EXPECT_FALSE(doc.setAnimationTime(0.5));
  EXPECT_FALSE(doc.setAnimationTime(std::numeric_limits<double>::infinity()));
  EXPECT_EQ(doc.currentFrameVersion(), version + 1);
}

TEST(AsyncSVGDocumentTest, AnimationClockRetriesContendedDocumentAccess) {
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));
  doc.document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);
  std::promise<void> acquired;
  std::promise<void> release;
  auto released = release.get_future();
  std::jthread reader([&] {
    const auto access = doc.document().readAccess();
    acquired.set_value();
    released.wait();
  });
  acquired.get_future().wait();
  EXPECT_FALSE(doc.setAnimationTime(0.5));
  EXPECT_FALSE(doc.restartAnimations());
  release.set_value();
  reader.join();
  EXPECT_TRUE(doc.setAnimationTime(0.5));
  EXPECT_EQ(doc.document().currentTime(), 0.5);
}

TEST(AsyncSVGDocumentTest, NewPlaybackSessionRestartsNeverAnimation) {
  constexpr std::string_view kAnimation = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <rect width="4" height="16" fill="red">
        <animate attributeName="width" from="4" to="12" dur="1s"
                 restart="never" fill="freeze"/>
      </rect>
    </svg>)svg";
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kAnimation));
  const auto handle = doc.document().handle();
  const std::string source(doc.document().source());
  EXPECT_THAT(PixelAt(RenderDocument(doc.document()), 8, 8), Rgba(0, 0, 0, 0));
  EXPECT_TRUE(doc.setAnimationTime(2.0));
  EXPECT_THAT(PixelAt(RenderDocument(doc.document()), 8, 8), Rgba(255, 0, 0, 255));
  EXPECT_TRUE(doc.restartAnimations());
  EXPECT_EQ(doc.document().currentTime(), 0.0);
  EXPECT_THAT(PixelAt(RenderDocument(doc.document()), 8, 8), Rgba(0, 0, 0, 0));
  EXPECT_TRUE(doc.setAnimationTime(2.0));
  EXPECT_THAT(PixelAt(RenderDocument(doc.document()), 8, 8), Rgba(255, 0, 0, 255));
  EXPECT_EQ(doc.document().handle(), handle);
  EXPECT_EQ(doc.document().source(), source);
}

TEST(AsyncSVGDocumentTest, MouseHoverAndPressEffectsPreserveAuthoredSource) {
  constexpr std::string_view source = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <rect id="pad" width="16" height="16" fill="red">
        <set attributeName="fill" to="blue" begin="mouseover" end="mouseout"/>
        <set attributeName="fill" to="lime" begin="mousedown" end="mouseup;mouseout"/>
      </rect>
    </svg>)svg";
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(source));
  const auto target = doc.document().querySelector("#pad");
  ASSERT_TRUE(target.has_value());
  EXPECT_THAT(PixelAt(RenderDocument(doc.document()), 8, 8), Rgba(255, 0, 0, 255));
  EXPECT_TRUE(doc.dispatchMouseEvent(svg::EventType::MouseOver, *target));
  EXPECT_THAT(PixelAt(RenderDocument(doc.document()), 8, 8), Rgba(0, 0, 255, 255));
  EXPECT_TRUE(doc.dispatchMouseEvent(svg::EventType::MouseDown, *target));
  EXPECT_THAT(PixelAt(RenderDocument(doc.document()), 8, 8), Rgba(0, 255, 0, 255));
  EXPECT_TRUE(doc.dispatchMouseEvent(svg::EventType::MouseUp, *target));
  EXPECT_THAT(PixelAt(RenderDocument(doc.document()), 8, 8), Rgba(0, 0, 255, 255));
  EXPECT_TRUE(doc.dispatchMouseEvent(svg::EventType::MouseOut, *target));
  EXPECT_THAT(PixelAt(RenderDocument(doc.document()), 8, 8), Rgba(255, 0, 0, 255));
  EXPECT_EQ(doc.document().source(), source);
  EXPECT_FALSE(doc.hasUnmirroredSourceDeltas());
}

TEST(AsyncSVGDocumentTest, MouseClickBubblesToNamedAnimationSourceAndRestarts) {
  constexpr std::string_view source = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <g id="button"><rect id="pad" width="2" height="2"/></g>
      <rect width="4" height="16" fill="red">
        <animate attributeName="width" from="4" to="12" begin="button.click" dur="1s" fill="freeze"/>
      </rect>
    </svg>)svg";
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(source));
  const auto target = doc.document().querySelector("#pad");
  ASSERT_TRUE(target.has_value());
  EXPECT_TRUE(doc.setAnimationTime(2.0));
  EXPECT_THAT(PixelAt(RenderDocument(doc.document()), 8, 8), Rgba(0, 0, 0, 0));
  EXPECT_TRUE(doc.dispatchMouseEvent(svg::EventType::Click, *target));
  EXPECT_TRUE(doc.setAnimationTime(3.0));
  EXPECT_THAT(PixelAt(RenderDocument(doc.document()), 8, 8), Rgba(255, 0, 0, 255));
  EXPECT_TRUE(doc.dispatchMouseEvent(svg::EventType::Click, *target));
  EXPECT_THAT(PixelAt(RenderDocument(doc.document()), 8, 8), Rgba(0, 0, 0, 0));
  EXPECT_EQ(doc.document().source(), source);
}

TEST(AsyncSVGDocumentTest, MouseEndDoesNotRetargetAnAlreadyFrozenInterval) {
  constexpr std::string_view source = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <rect id="pad" width="4" height="16" fill="red">
        <animate attributeName="width" from="4" to="12" begin="click" end="mouseup" dur="4s" fill="freeze"/>
      </rect>
    </svg>)svg";
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(source));
  const auto target = doc.document().querySelector("#pad");
  ASSERT_TRUE(target.has_value());
  EXPECT_TRUE(doc.dispatchMouseEvent(svg::EventType::Click, *target));
  EXPECT_TRUE(doc.setAnimationTime(1.0));
  EXPECT_TRUE(doc.dispatchMouseEvent(svg::EventType::MouseUp, *target));
  EXPECT_THAT(PixelAt(RenderDocument(doc.document()), 8, 8), Rgba(0, 0, 0, 0));
  EXPECT_TRUE(doc.setAnimationTime(3.0));
  EXPECT_FALSE(doc.dispatchMouseEvent(svg::EventType::MouseUp, *target));
  EXPECT_THAT(PixelAt(RenderDocument(doc.document()), 8, 8), Rgba(0, 0, 0, 0));
}

TEST(AsyncSVGDocumentTest, MouseEventsRejectForeignTargetsAndSecureDocuments) {
  constexpr std::string_view source = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <rect id="pad" width="16" height="16"><set attributeName="fill" to="red" begin="click"/></rect>
    </svg>)svg";
  AsyncSVGDocument first;
  AsyncSVGDocument second;
  ASSERT_TRUE(first.loadFromString(source));
  ASSERT_TRUE(second.loadFromString(source));
  const auto foreignTarget = second.document().querySelector("#pad");
  ASSERT_TRUE(foreignTarget.has_value());
  EXPECT_FALSE(first.dispatchMouseEvent(svg::EventType::Click, *foreignTarget));
  ParseWarningSink sink = ParseWarningSink::Disabled();
  svg::parser::SVGParser::Options options;
  options.enableExperimental = true;
  auto parsed = svg::parser::SVGParser::ParseSVG(
      source, sink, options, {.processingMode = svg::ProcessingMode::SecureAnimated});
  ASSERT_FALSE(parsed.hasError());
  auto secure = std::move(parsed).result();
  const auto target = secure.querySelector("#pad");
  ASSERT_TRUE(target.has_value());
  EXPECT_FALSE(secure.dispatchMouseEvent(svg::EventType::Click, *target));
}

TEST(AsyncSVGDocumentTest, CapturedMouseTargetsStayWithTheirDisplayedGeometry) {
  constexpr std::string_view source = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <rect id="moving" width="4" height="16" fill="red">
        <animate attributeName="x" from="0" to="12" dur="1s" fill="freeze"/>
      </rect>
    </svg>)svg";
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(source));
  svg::DonnerController controller(doc.document());
  const auto oldFrame = controller.captureHitTest();
  ASSERT_TRUE(oldFrame->complete());
  const auto originalHit = oldFrame->findIntersecting(Vector2d(2, 8));
  ASSERT_TRUE(originalHit.has_value());
  EXPECT_TRUE(doc.setAnimationTime(1.0));
  const auto newFrame = controller.captureHitTest();
  EXPECT_EQ(newFrame->findIntersecting(Vector2d(2, 8)), std::nullopt);
  EXPECT_EQ(oldFrame->findIntersecting(Vector2d(2, 8)), originalHit);
  EXPECT_EQ(newFrame->findIntersecting(Vector2d(14, 8)), originalHit);
}

TEST(AsyncSVGDocumentTest, CapturedMouseBudgetRejectsTheWholeSnapshot) {
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <rect width="16" height="16" fill="red"/>
      <circle cx="8" cy="8" r="4" fill="blue"/>
    </svg>)svg"));
  const auto access = doc.document().writeAccess();
  svg::components::RenderingContext context(access.registry());
  ASSERT_TRUE(context.captureHitGeometry().has_value());
  EXPECT_EQ(context.captureHitGeometry(1), std::nullopt);
  EXPECT_EQ(context.captureHitGeometry(65536, 0), std::nullopt);
}

TEST(AsyncSVGDocumentTest, SuccessfulParsePublishesWarnings) {
  constexpr std::string_view kSvgWithWarning = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" xmlns:unused="urn:unexpected">
      <rect width="10" height="10"/>
    </svg>)svg";
  AsyncSVGDocument doc;

  ASSERT_TRUE(doc.loadFromString(kSvgWithWarning));

  ASSERT_FALSE(doc.parseDiagnostics().empty());
  EXPECT_EQ(doc.parseDiagnostics().front().severity, DiagnosticSeverity::Warning);
  EXPECT_FALSE(doc.lastParseError().has_value());
  EXPECT_GT(doc.parseDiagnosticsRevision(), 0u);
}

TEST(AsyncSVGDocumentTest, ParseDiagnosticsAdvanceAndClearOnNextSuccessfulParse) {
  AsyncSVGDocument doc;
  ASSERT_FALSE(doc.loadFromString("<svg"));
  const std::uint64_t failedRevision = doc.parseDiagnosticsRevision();
  ASSERT_EQ(doc.parseDiagnostics().size(), 1u);
  EXPECT_EQ(doc.parseDiagnostics().front().severity, DiagnosticSeverity::Error);

  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));

  EXPECT_GT(doc.parseDiagnosticsRevision(), failedRevision);
  EXPECT_TRUE(doc.parseDiagnostics().empty());
  EXPECT_FALSE(doc.lastParseError().has_value());
}

TEST(AsyncSVGDocumentTest, FlushAppliesQueuedSetTransform) {
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));
  const auto initialVersion = doc.currentFrameVersion();

  auto rect = doc.document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());

  const Transform2d translateBy = Transform2d::Translate(Vector2d(7.0, 11.0));
  doc.applyMutation(EditorCommand::SetTransformCommand(*rect, translateBy));

  EXPECT_TRUE(doc.flushFrame());
  EXPECT_GT(doc.currentFrameVersion(), initialVersion);

  auto graphicsElement = rect->cast<svg::SVGGraphicsElement>();
  const Transform2d after = graphicsElement.transform();
  EXPECT_THAT(after.translation(), Vector2dNear(Vector2d(7.0, 11.0), 1e-9));
}

TEST(AsyncSVGDocumentTest, MultipleSetTransformsCoalesceAtFlush) {
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));

  auto rect = doc.document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());

  // Three writes, only the last should apply.
  doc.applyMutation(
      EditorCommand::SetTransformCommand(*rect, Transform2d::Translate(Vector2d(1.0, 0.0))));
  doc.applyMutation(
      EditorCommand::SetTransformCommand(*rect, Transform2d::Translate(Vector2d(2.0, 0.0))));
  doc.applyMutation(
      EditorCommand::SetTransformCommand(*rect, Transform2d::Translate(Vector2d(3.0, 0.0))));
  EXPECT_EQ(doc.queue().size(), 3u);

  EXPECT_TRUE(doc.flushFrame());

  auto graphicsElement = rect->cast<svg::SVGGraphicsElement>();
  EXPECT_THAT(graphicsElement.transform().translation(), Vector2dNear(Vector2d(3.0, 0.0), 1e-9));
}

TEST(AsyncSVGDocumentTest, ReplaceDocumentSwapsTheTreeAndDropsPriorMutations) {
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));
  ASSERT_TRUE(doc.document().querySelector("#r1").has_value());

  auto rect = doc.document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());

  // Queue a SetTransform, then a ReplaceDocument. The SetTransform must
  // be dropped because its target entity belongs to the doomed document.
  doc.applyMutation(
      EditorCommand::SetTransformCommand(*rect, Transform2d::Translate(Vector2d(99.0, 99.0))));
  doc.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(kReplacementSvg)));

  EXPECT_TRUE(doc.flushFrame());
  EXPECT_TRUE(doc.lastFlushResult().replacedDocument);
  EXPECT_FALSE(doc.lastFlushResult().preserveUndoOnReparse);

  EXPECT_FALSE(doc.document().querySelector("#r1").has_value());
  EXPECT_TRUE(doc.document().querySelector("#r2").has_value());
}

TEST(AsyncSVGDocumentTest, PreserveUndoMetadataSurvivesWritebackReplaceDocument) {
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));

  doc.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(kReplacementSvg), true));

  EXPECT_TRUE(doc.flushFrame());
  EXPECT_TRUE(doc.lastFlushResult().appliedCommands);
  EXPECT_TRUE(doc.lastFlushResult().replacedDocument);
  EXPECT_TRUE(doc.lastFlushResult().preserveUndoOnReparse);
}

TEST(AsyncSVGDocumentTest, CommandAfterStructuralWritebackReparseTargetsRemappedElement) {
  constexpr std::string_view kInitialSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <path id="p" d="M 10 10 L 80 10 L 10 80 Z" style="fill: none; stroke: none"/>
         </svg>)svg";
  constexpr std::string_view kWritebackSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <path id="p" d="M 10 10 L 80 10 L 10 80 Z" style="fill: none; stroke: none"/>
         </svg>)svg";

  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kInitialSvg));
  auto pathBeforeReparse = doc.document().querySelector("#p");
  ASSERT_TRUE(pathBeforeReparse.has_value());

  doc.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(kWritebackSvg),
                                                          /*preserveUndoOnReparse=*/true));
  doc.applyMutation(EditorCommand::SetAttributeCommand(*pathBeforeReparse, "style",
                                                       "fill: #ff0000; stroke: none"));

  ASSERT_TRUE(doc.flushFrame());
  ASSERT_TRUE(doc.lastFlushResult().replacedDocument);
  ASSERT_TRUE(doc.lastFlushResult().preserveUndoOnReparse);

  auto pathAfterReparse = doc.document().querySelector("#p");
  ASSERT_TRUE(pathAfterReparse.has_value());
  EXPECT_EQ(pathAfterReparse->getAttribute("style"), "fill: #ff0000; stroke: none");

  const svg::RendererBitmap rendered = RenderDocument(doc.document());
  ASSERT_FALSE(rendered.empty());
  const std::array<std::uint8_t, 4> pixel = PixelAt(rendered, 24, 24);
  EXPECT_THAT(pixel, Rgba(Gt(200), Lt(40), Lt(40), Gt(200)));
}

TEST(AsyncSVGDocumentTest, StructuralWritebackPreservesCanvasSize) {
  constexpr std::string_view kMovedSameTreeSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <rect id="r1" x="0" y="0" width="10" height="10" fill="red" transform="translate(7,0)"/>
         </svg>)svg";

  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));
  doc.document().setCanvasSize(200, 200);

  doc.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(kMovedSameTreeSvg),
                                                          /*preserveUndoOnReparse=*/true));
  ASSERT_TRUE(doc.flushFrame());

  EXPECT_EQ(doc.document().canvasSize(), Vector2i(200, 200));
  EXPECT_FALSE(doc.consumePendingStructuralRemap().empty());
}

TEST(AsyncSVGDocumentTest, SourceReplacementPreservesStructurallyEquivalentEntitySpace) {
  constexpr std::string_view kStyledSameTreeSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <rect id="r1" x="0" y="0" width="10" height="10" fill="blue"/>
         </svg>)svg";

  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));
  doc.document().setCanvasSize(200, 200);

  doc.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(kStyledSameTreeSvg),
                                                          /*preserveUndoOnReparse=*/false));
  ASSERT_TRUE(doc.flushFrame());

  EXPECT_FALSE(doc.lastFlushResult().preserveUndoOnReparse);
  EXPECT_EQ(doc.document().canvasSize(), Vector2i(200, 200));
  EXPECT_FALSE(doc.consumePendingStructuralRemap().empty());
}

TEST(AsyncSVGDocumentTest, MixedReplaceDocumentBatchClearsPreserveUndoMetadata) {
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));

  doc.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(kReplacementSvg), true));
  doc.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(kTrivialSvg)));

  EXPECT_TRUE(doc.flushFrame());
  EXPECT_TRUE(doc.lastFlushResult().replacedDocument);
  EXPECT_FALSE(doc.lastFlushResult().preserveUndoOnReparse);
}

TEST(AsyncSVGDocumentTest, FlushIsNoOpWhenQueueIsEmpty) {
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));
  const auto versionAfterLoad = doc.currentFrameVersion();

  EXPECT_FALSE(doc.flushFrame());
  EXPECT_EQ(doc.currentFrameVersion(), versionAfterLoad);
}

TEST(AsyncSVGDocumentTest, FailedLoadStashesParseErrorAndKeepsOldDocument) {
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));
  EXPECT_FALSE(doc.lastParseError().has_value());

  // Truly malformed XML - unclosed tag, no '>'.
  EXPECT_FALSE(doc.loadFromString("<svg xmlns=\"http://www.w3.org/2000/svg\""));
  ASSERT_TRUE(doc.lastParseError().has_value());

  // The original document is still intact and queryable so the user can
  // keep editing on top of the prior valid state.
  EXPECT_TRUE(doc.hasDocument());
  EXPECT_TRUE(doc.document().querySelector("#r1").has_value());
}

TEST(AsyncSVGDocumentTest, SuccessfulReloadClearsParseError) {
  AsyncSVGDocument doc;
  EXPECT_FALSE(doc.loadFromString("<svg xmlns=\"http://www.w3.org/2000/svg\""));
  ASSERT_TRUE(doc.lastParseError().has_value());

  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));
  EXPECT_FALSE(doc.lastParseError().has_value());
}

}  // namespace
}  // namespace donner::editor
