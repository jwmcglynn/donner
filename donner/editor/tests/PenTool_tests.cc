#include "donner/editor/PenTool.h"

#include <chrono>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/DocumentSyncController.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/TextEditor.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGPathElement.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

// gmock matcher: a Box2d encloses the given document-space point. On failure it
// prints the full box (topLeft => bottomRight) and the point so the off-by-one
// stale-bounds failure is diagnosable without a rerun.
MATCHER_P(EnclosesPoint, point, "") {
  const bool ok = arg.contains(point);
  *result_listener << "box " << arg.topLeft << " => " << arg.bottomRight
                   << (ok ? " contains " : " does NOT contain ") << point;
  return ok;
}

constexpr std::string_view kEmptySvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"></svg>)";

constexpr std::string_view kSelfClosingEmptySvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"/>)";

constexpr std::string_view kXmlDeclarationSvg =
    R"(<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <g id="layer"></g>
</svg>)";

constexpr std::string_view kOpenPathSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <path id="p" d="M 0 0 L 10 0" fill="none" stroke="black"/>
       </svg>)";

class PenToolTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_TRUE(app.loadFromString(kEmptySvg)); }

  svg::SVGPathElement path() {
    auto element = app.document().document().querySelector("path");
    EXPECT_TRUE(element.has_value());
    return element->cast<svg::SVGPathElement>();
  }

  EditorApp app;
  PenTool tool;
};

TEST_F(PenToolTest, FirstClickInsertsSelectedPath) {
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  svg::SVGPathElement inserted = path();
  EXPECT_EQ(inserted.d(), "M 10 20");
  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front(), inserted);
  const std::string source(app.document().document().source());
  const std::size_t pathOffset = source.find(R"(<path d="M 10 20")");
  const std::size_t svgCloseOffset = source.find("</svg>");
  ASSERT_NE(pathOffset, std::string::npos);
  ASSERT_NE(svgCloseOffset, std::string::npos);
  EXPECT_LT(pathOffset, svgCloseOffset);
}

TEST_F(PenToolTest, FirstClickUsesActivePaintStyle) {
  app.setActiveFill("#112233");
  app.setActiveStroke("#445566");
  app.setActiveStrokeWidth(2.5);

  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  svg::SVGPathElement inserted = path();
  EXPECT_EQ(inserted.getAttribute("fill"), "#112233");
  EXPECT_EQ(inserted.getAttribute("stroke"), "#445566");
  EXPECT_EQ(inserted.getAttribute("stroke-width"), "2.5");
}

TEST_F(PenToolTest, FirstClickExpandsSelfClosingSvgRoot) {
  ASSERT_TRUE(app.loadFromString(kSelfClosingEmptySvg));

  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  const std::string source(app.document().document().source());
  const std::size_t pathOffset = source.find(R"(<path d="M 10 20")");
  const std::size_t svgCloseOffset = source.find("</svg>");
  ASSERT_NE(pathOffset, std::string::npos);
  ASSERT_NE(svgCloseOffset, std::string::npos);
  EXPECT_LT(pathOffset, svgCloseOffset);
  EXPECT_NE(source.find(R"(height="100"><path)"), std::string::npos);
}

TEST_F(PenToolTest, FirstClickInsertsInsideSvgWithXmlDeclaration) {
  ASSERT_TRUE(app.loadFromString(kXmlDeclarationSvg));

  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  const std::string source(app.document().document().source());
  const std::size_t pathOffset = source.find(R"(<path d="M 10 20")");
  const std::size_t svgCloseOffset = source.find("</svg>");
  ASSERT_NE(pathOffset, std::string::npos);
  ASSERT_NE(svgCloseOffset, std::string::npos);
  EXPECT_LT(pathOffset, svgCloseOffset);
}

TEST_F(PenToolTest, FirstClickInsertsInsideDonnerSplashRoot) {
  std::ifstream splashStream("donner_splash.svg");
  ASSERT_TRUE(splashStream.is_open());
  std::ostringstream splashBuffer;
  splashBuffer << splashStream.rdbuf();
  const std::string splashSource = splashBuffer.str();
  ASSERT_TRUE(app.loadFromString(splashSource));

  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  const std::string source(app.document().document().source());
  const std::size_t pathOffset = source.find(R"(<path d="M 10 20")");
  const std::size_t svgCloseOffset = source.rfind("</svg>");
  ASSERT_NE(pathOffset, std::string::npos);
  ASSERT_NE(svgCloseOffset, std::string::npos);
  EXPECT_LT(pathOffset, svgCloseOffset);
}

TEST_F(PenToolTest, SubsequentClicksAppendLineSegments) {
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  tool.onMouseDown(app, Vector2d(30.0, 40.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(path().d(), "M 10 20 L 30 40");
  EXPECT_TRUE(tool.isDrafting());
}

TEST_F(PenToolTest, BoundsIncludeNewestPointAfterImmediateIdleFlush) {
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  tool.onMouseDown(app, Vector2d(80.0, 90.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  const std::vector<Box2d> bounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(app.selectedElements()));
  ASSERT_EQ(bounds.size(), 1u);
  EXPECT_LE(bounds.front().topLeft.x, 10.0);
  EXPECT_LE(bounds.front().topLeft.y, 20.0);
  EXPECT_GE(bounds.front().bottomRight.x, 80.0);
  EXPECT_GE(bounds.front().bottomRight.y, 90.0);
}

TEST_F(PenToolTest, ConsecutiveClicksBeforeFlushStayInsideSvgRoot) {
  TextEditor textEditor;
  textEditor.setText(kEmptySvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kEmptySvg)};
  SelectTool selectTool;

  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  tool.onMouseDown(app, Vector2d(30.0, 40.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  controller.applyPendingWritebacks(app, selectTool, textEditor);

  EXPECT_EQ(textEditor.getText(), app.document().document().source());
  const std::size_t pathOffset = textEditor.getText().find(R"(d="M 10 20 L 30 40")");
  const std::size_t svgCloseOffset = textEditor.getText().find("</svg>");
  ASSERT_NE(pathOffset, std::string::npos);
  ASSERT_NE(svgCloseOffset, std::string::npos);
  EXPECT_LT(pathOffset, svgCloseOffset);
}

// Regression for the user-reported "AABB overlay for the pen tool's drawn path
// is one point out of date" bug. The selection-bounds overlay drew the pen
// path's AABB from `SnapshotSelectionWorldBounds`, which reads the LIVE DOM `d`
// attribute. But `PenTool::appendLine` only *queues* a `SetAttribute("d")`
// command — the live DOM does not reflect the just-placed point until the NEXT
// `flushFrame()`. So in the same click frame the AABB excluded point N, lagging
// one click behind. This test places point N WITHOUT an intervening flush and
// proves: (a) the stale live-DOM snapshot the overlay used does NOT enclose N
// (the bug), and (b) the synchronous `PenTool::draftBounds()` the overlay now
// uses DOES enclose N in the same frame.
TEST_F(PenToolTest, PenDraftBoundsEncloseJustPlacedPointSameFrame) {
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  // Place point N (80, 90) but do NOT flush — this is the same click frame the
  // overlay renders in.
  tool.onMouseDown(app, Vector2d(80.0, 90.0), MouseModifiers{});

  const Vector2d justPlaced(80.0, 90.0);

  // The stale path the overlay used before the fix: the live-DOM snapshot still
  // has only the first point because the SetAttribute("d") is still queued.
  const std::vector<Box2d> staleBounds =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(app.selectedElements()));
  ASSERT_EQ(staleBounds.size(), 1u);
  EXPECT_THAT(staleBounds.front(), ::testing::Not(EnclosesPoint(justPlaced)))
      << "live-DOM snapshot should still lag the queued point (demonstrates the bug)";

  // The fix: the synchronous draft bounds reflect the just-placed point in the
  // same frame, with no extra flush.
  const std::optional<Box2d> draftBounds = tool.draftBounds();
  ASSERT_TRUE(draftBounds.has_value());
  EXPECT_THAT(*draftBounds, EnclosesPoint(justPlaced));
  EXPECT_THAT(*draftBounds, EnclosesPoint(Vector2d(10.0, 20.0)));
}

TEST_F(PenToolTest, DraggingPlacedPointCreatesCubicHandles) {
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(20.0, 10.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(20.0, 10.0));
  ASSERT_TRUE(app.flushFrame());

  tool.onMouseDown(app, Vector2d(40.0, 10.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(40.0, 30.0), /*buttonHeld=*/true);

  ASSERT_EQ(tool.previewSegments().size(), 1u);
  EXPECT_TRUE(tool.previewSegments().front().cubic);
  EXPECT_EQ(tool.activePathData(), "M 10 10 C 20 10 40 -10 40 10");

  tool.onMouseUp(app, Vector2d(40.0, 30.0));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(path().d(), "M 10 10 C 20 10 40 -10 40 10");
}

TEST_F(PenToolTest, ClickNearStartClosesPath) {
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  tool.onMouseDown(app, Vector2d(40.0, 10.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  tool.onMouseDown(app, Vector2d(40.0, 40.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  MouseModifiers modifiers;
  modifiers.pixelsPerDocUnit = 1.0;
  tool.onMouseDown(app, Vector2d(11.0, 10.0), modifiers);
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(path().d(), "M 10 10 L 40 10 L 40 40 Z");
  EXPECT_FALSE(tool.isDrafting());
}

TEST_F(PenToolTest, ShiftConstrainsNextSegment) {
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  MouseModifiers modifiers;
  modifiers.shift = true;
  tool.onMouseDown(app, Vector2d(40.0, 18.0), modifiers);
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(path().d(), "M 10 10 L 40 10");
}

TEST_F(PenToolTest, SelectedOpenPathCanBeContinued) {
  ASSERT_TRUE(app.loadFromString(kOpenPathSvg));
  auto selected = app.document().document().querySelector("#p");
  ASSERT_TRUE(selected.has_value());
  app.setSelection(*selected);

  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(selected->cast<svg::SVGPathElement>().d(), "M 0 0 L 10 0 L 10 10");
  EXPECT_TRUE(tool.isDrafting());
}

// Regression for the live-editor Pen crash: clicking the Pen tool while a
// `<path>` is selected aborted with
//   UTILS_RELEASE_ASSERT failed: documentHandle_->currentThreadHasAccess()
// because `PenTool::openStateForSelectedPath` read `SVGPathElement::d()` (raw
// ECS access through an SVGElement) without holding a ConcurrentDom read-access
// scope. The unit tests above never tripped it because a freshly loaded
// document is `ThreadingMode::SingleThreaded`, where `currentThreadHasAccess()`
// is always true. The live editor flips the document to
// `ThreadingMode::ConcurrentDom` on its first async render (see
// AsyncRenderer.cc), which is the state the user was in. This test reproduces
// the user's exact sequence — select a path, enter ConcurrentDom, Pen
// onMouseDown — so the guarded read is actually exercised.
TEST_F(PenToolTest, SelectedOpenPathContinuesUnderConcurrentDom) {
  ASSERT_TRUE(app.loadFromString(kOpenPathSvg));
  auto selected = app.document().document().querySelector("#p");
  ASSERT_TRUE(selected.has_value());
  app.setSelection(*selected);

  // Mirror the live editor: the document is in ConcurrentDom mode by the time
  // the user clicks (the async renderer flips it on first render). The UI
  // thread holds no read/write access scope when a tool's onMouseDown fires.
  app.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);
  ASSERT_EQ(app.document().document().threadingMode(), svg::ThreadingMode::ConcurrentDom);

  // At c0e8c53f this aborts via the release assert inside SVGPathElement::d().
  // After the fix the selected-path read is wrapped in a read-access scope and
  // the path continues normally.
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  // The test verifies the result the same way the live editor reads the selected
  // element under ConcurrentDom — through a scoped read-access guard (the
  // SelectTool / OverlayRenderer idiom). Reading `d()` raw here would itself trip
  // the ConcurrentDom release assert; the production crash is in the tool's
  // onMouseDown path above, which now runs to completion.
  const std::string resolvedPathData =
      selected->withReadAccess([&selected](svg::DocumentReadAccess&, EntityHandle) {
        return std::string(std::string_view(selected->cast<svg::SVGPathElement>().d()));
      });
  EXPECT_EQ(resolvedPathData, "M 0 0 L 10 0 L 10 10");
  EXPECT_TRUE(tool.isDrafting());
}

// Sibling of the regression above for the first-click-on-empty-canvas path.
// With nothing selected, `onMouseDown` falls through to `startNewPath`, which
// synchronously creates the `<path>`, writes its `d`/`fill`/`stroke`
// attributes, resolves the root `<svg>`, and inserts — all raw ECS
// reads/writes through SVGElement. Under ThreadingMode::ConcurrentDom (the
// live editor's steady state) those accesses abort via SVGElement's
// scoped-access release assert unless the tool holds a document write scope.
// startNewPath wraps the whole create/setAttribute/insert sequence in
// withWriteAccess to satisfy that guard.
TEST_F(PenToolTest, FirstClickStartsNewPathUnderConcurrentDom) {
  app.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);
  ASSERT_EQ(app.document().document().threadingMode(), svg::ThreadingMode::ConcurrentDom);

  // At base 667cf509 this aborts via the release assert inside
  // SVGPathElement::Create / setAttribute (raw ECS writes with no scope held).
  // After the fix startNewPath's write-scope wrapper lets it run to completion.
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  ASSERT_EQ(app.selectedElements().size(), 1u);
  // Verify the result through a scoped read guard, exactly as the live editor
  // reads the selected element under ConcurrentDom (raw `d()` here would itself
  // trip the release assert).
  const svg::SVGElement inserted = app.selectedElements().front();
  const std::string insertedPathData =
      inserted.withReadAccess([&inserted](svg::DocumentReadAccess&, EntityHandle) {
        return std::string(std::string_view(inserted.cast<svg::SVGPathElement>().d()));
      });
  EXPECT_EQ(insertedPathData, "M 10 20");
  EXPECT_TRUE(tool.isDrafting());
}

TEST_F(PenToolTest, GenericSourceDeltasMirrorPenChangesIntoTextPane) {
  TextEditor textEditor;
  textEditor.setText(kEmptySvg);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kEmptySvg)};
  SelectTool selectTool;

  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  controller.applyPendingWritebacks(app, selectTool, textEditor);

  EXPECT_EQ(textEditor.getText(), app.document().document().source());
  const std::size_t pathOffset = textEditor.getText().find(R"(<path d="M 10 20")");
  const std::size_t svgCloseOffset = textEditor.getText().find("</svg>");
  ASSERT_NE(pathOffset, std::string::npos);
  ASSERT_NE(svgCloseOffset, std::string::npos);
  EXPECT_LT(pathOffset, svgCloseOffset);

  tool.onMouseDown(app, Vector2d(30.0, 40.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  controller.applyPendingWritebacks(app, selectTool, textEditor);

  EXPECT_EQ(textEditor.getText(), app.document().document().source());
  EXPECT_NE(textEditor.getText().find(R"(d="M 10 20 L 30 40")"), std::string::npos);
}

TEST_F(PenToolTest, FirstClickInDirtyTextPaneStaysInsideCurrentSvgRoot) {
  constexpr std::string_view kSvgWithRect =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><rect id="old"/></svg>)";
  constexpr std::string_view kDirtySvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"></svg>)";

  ASSERT_TRUE(app.loadFromString(kSvgWithRect));
  TextEditor textEditor;
  textEditor.setText(kDirtySvg);
  textEditor.resetTextChanged();
  ASSERT_EQ(textEditor.getText(), kDirtySvg);
  DocumentSyncController controller{std::string(kSvgWithRect)};
  SelectTool selectTool;

  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  controller.applyPendingWritebacks(app, selectTool, textEditor);

  const std::string sourceText = textEditor.getText();
  const std::size_t pathOffset = sourceText.find(R"(<path d="M 10 20")");
  const std::size_t svgCloseOffset = sourceText.find("</svg>");
  ASSERT_NE(pathOffset, std::string::npos);
  ASSERT_NE(svgCloseOffset, std::string::npos);
  EXPECT_LT(pathOffset, svgCloseOffset) << sourceText;
  EXPECT_EQ(sourceText.find(R"(id="old")"), std::string::npos) << sourceText;

  ASSERT_TRUE(app.flushFrame());
  controller.applyPendingWritebacks(app, selectTool, textEditor);

  EXPECT_EQ(textEditor.getText(), app.document().document().source());
  EXPECT_FALSE(app.document().document().querySelector("#old").has_value());
  EXPECT_TRUE(app.document().document().querySelector("path").has_value());
}

TEST_F(PenToolTest, DirtyTextPaneWithTrailingRootLikeCommentKeepsPathInsideSvgRoot) {
  constexpr std::string_view kSvgWithRectAndTail =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><rect id="old"/></svg>
<!-- saved root close marker: </svg> -->)";
  constexpr std::string_view kDirtySvgWithTail =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"></svg>
<!-- saved root close marker: </svg> -->)";

  ASSERT_TRUE(app.loadFromString(kSvgWithRectAndTail));
  TextEditor textEditor;
  textEditor.setText(kDirtySvgWithTail);
  textEditor.resetTextChanged();
  DocumentSyncController controller{std::string(kSvgWithRectAndTail)};
  SelectTool selectTool;

  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  controller.applyPendingWritebacks(app, selectTool, textEditor);

  const std::string sourceText = textEditor.getText();
  const std::size_t pathOffset = sourceText.find(R"(<path d="M 10 20")");
  const std::size_t svgCloseOffset = sourceText.find("</svg>");
  ASSERT_NE(pathOffset, std::string::npos);
  ASSERT_NE(svgCloseOffset, std::string::npos);
  EXPECT_LT(pathOffset, svgCloseOffset) << sourceText;
  EXPECT_EQ(sourceText.find("<path", svgCloseOffset), std::string::npos) << sourceText;
}

}  // namespace
}  // namespace donner::editor
