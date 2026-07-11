#include "donner/editor/AsyncSVGDocument.h"

#include <gmock/gmock.h>

#include <array>

#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/renderer/Renderer.h"
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
