#include "donner/editor/PenTool.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/DocumentSyncController.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/TextEditor.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

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

MATCHER_P(BoxContainingPoint, point,
          std::string("a Box2d that contains (") + testing::PrintToString(point.x) + ", " +
              testing::PrintToString(point.y) + ")") {
  if (arg.contains(point)) {
    return true;
  }
  *result_listener << "box [" << arg.topLeft.x << ", " << arg.topLeft.y << " — "
                   << arg.bottomRight.x << ", " << arg.bottomRight.y << "] does not contain ("
                   << point.x << ", " << point.y << ")";
  return false;
}

/// Capture pen-path overlay chrome the same way the editor does on a click frame.
SelectionChromeSnapshot CapturePenChromeSnapshot(EditorApp& app) {
  return OverlayRenderer::captureChromeSnapshot(
      std::span<const svg::SVGElement>(app.selectedElements()), std::nullopt, Transform2d(),
      std::nullopt, std::span<const svg::SVGElement>(), std::nullopt,
      SelectionChromeDetail::PathOutlinesOnly);
}

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
  // Paint is seeded as a single inline `style` attribute (CSS), not as
  // individual fill/stroke/stroke-width presentation attributes.
  EXPECT_EQ(inserted.getAttribute("style"), "fill: #112233; stroke: #445566; stroke-width: 2.5");
  EXPECT_FALSE(inserted.getAttribute("fill").has_value());
  EXPECT_FALSE(inserted.getAttribute("stroke").has_value());
  EXPECT_FALSE(inserted.getAttribute("stroke-width").has_value());
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

TEST_F(PenToolTest, OverlayChromeIncludesNewAnchorOnSameFlush) {
  // First click places the initial anchor; capturing chrome here mirrors the
  // editor capturing overlay chrome on the click frame and instantiates the
  // cached computed path for the new <path>.
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  (void)CapturePenChromeSnapshot(app);
  tool.onMouseUp(app, Vector2d(10.0, 20.0));

  // Second click appends a segment. Chrome captured after the same flush — with
  // no async render in between — must already include the new segment's anchor.
  // A stale capture here is the user-visible "overlay only updates on the next
  // mousemove" bug.
  tool.onMouseDown(app, Vector2d(30.0, 40.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  tool.onMouseUp(app, Vector2d(30.0, 40.0));

  const SelectionChromeSnapshot snapshot = CapturePenChromeSnapshot(app);
  ASSERT_FALSE(snapshot.paths.empty());
  EXPECT_THAT(snapshot.pathAnchorBoxesDoc,
              testing::Contains(BoxContainingPoint(Vector2d(30.0, 40.0))))
      << "Overlay chrome captured on the click's own flush must contain the newly placed anchor.";
}

TEST_F(PenToolTest, SubsequentClicksAppendLineSegments) {
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  tool.onMouseDown(app, Vector2d(30.0, 40.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(path().d(), "M 10 20 L 30 40");
  EXPECT_TRUE(tool.isDrafting());
}

TEST_F(PenToolTest, RemoveLastAnchorPopsNewestPoint) {
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  tool.onMouseDown(app, Vector2d(30.0, 40.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  tool.onMouseDown(app, Vector2d(50.0, 60.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(50.0, 60.0));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_EQ(path().d(), "M 10 20 L 30 40 L 50 60");

  ASSERT_TRUE(tool.removeLastAnchor(app));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(path().d(), "M 10 20 L 30 40");
  EXPECT_TRUE(tool.isDrafting()) << "removing an anchor keeps the draft session alive";
}

TEST_F(PenToolTest, RemoveLastAnchorOnLoneAnchorDiscardsDraft) {
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(10.0, 20.0));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_TRUE(tool.isDrafting());

  ASSERT_TRUE(tool.removeLastAnchor(app));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_FALSE(tool.isDrafting());
  EXPECT_FALSE(app.hasSelection()) << "discarding a lone-anchor draft removes the created path";
}

TEST_F(PenToolTest, RemoveLastAnchorIsNoOpDuringDrag) {
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  tool.onMouseDown(app, Vector2d(30.0, 40.0), MouseModifiers{});
  ASSERT_TRUE(tool.isDraggingAnchor());

  EXPECT_FALSE(tool.removeLastAnchor(app))
      << "an active drag owns the newest anchor; Backspace must not yank it mid-gesture";
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

TEST_F(PenToolTest, DraggingPlacedPointCreatesCubicHandles) {
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(20.0, 10.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(20.0, 10.0));
  ASSERT_TRUE(app.flushFrame());

  tool.onMouseDown(app, Vector2d(40.0, 10.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(40.0, 30.0), /*buttonHeld=*/true);

  EXPECT_EQ(tool.activePathData(), "M 10 10 C 20 10 40 -10 40 10");

  tool.onMouseUp(app, Vector2d(40.0, 30.0));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(path().d(), "M 10 10 C 20 10 40 -10 40 10");
}

TEST_F(PenToolTest, DraggingAnchorUpdatesDomPathBeforeMouseUp) {
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(10.0, 10.0));
  ASSERT_TRUE(app.flushFrame());

  tool.onMouseDown(app, Vector2d(40.0, 10.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(40.0, 30.0), /*buttonHeld=*/true);
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(path().d(), "M 10 10 C 10 10 40 -10 40 10")
      << "The real SVG path must update while the anchor drag is in progress, before mouseup.";
}

TEST_F(PenToolTest, RepeatedAnchorDragAtSamePointDoesNotQueueDuplicatePathMutation) {
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(10.0, 10.0));
  ASSERT_TRUE(app.flushFrame());

  tool.onMouseDown(app, Vector2d(40.0, 10.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  tool.onMouseMove(app, Vector2d(40.0, 30.0), /*buttonHeld=*/true);
  ASSERT_TRUE(app.document().hasPendingMutations());
  ASSERT_TRUE(app.flushFrame());
  const std::uint64_t versionAfterDrag = app.document().currentFrameVersion();

  tool.onMouseMove(app, Vector2d(40.0, 30.0), /*buttonHeld=*/true);
  EXPECT_FALSE(app.document().hasPendingMutations())
      << "Holding the mouse still must not keep the live document ahead of the overlay.";
  EXPECT_EQ(app.document().currentFrameVersion(), versionAfterDrag);
  EXPECT_EQ(path().d(), "M 10 10 C 10 10 40 -10 40 10");
}

// Hardening guard for the user-reported "pen path placed after </svg> so it
// never renders" symptom. The existing tests only assert source-substring order
// right after onMouseDown+flushFrame; this drives the FULL pen session through
// finalize (commitOpenPath — the Enter / tool-switch commit path), flushes, and
// then *re-parses* the serialized source to prove the new <path> is a direct
// child of the root <svg> (i.e. it actually renders), not merely that its bytes
// precede </svg>. The splice was already correct at the time this test was
// added; the test locks in that the finalize path keeps the path inside the
// root so the after-</svg> regression cannot silently return.
TEST_F(PenToolTest, FinalizedPenPathStaysInsideSvgRootSource) {
  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  tool.onMouseDown(app, Vector2d(30.0, 40.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  tool.onMouseDown(app, Vector2d(60.0, 70.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  // Finalize the open path exactly as Enter / a tool switch does.
  ASSERT_TRUE(tool.commitOpenPath(app));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_FALSE(tool.isDrafting());

  const std::string source(app.document().document().source());
  const std::size_t pathOffset = source.find("<path");
  const std::size_t svgCloseOffset = source.rfind("</svg>");
  ASSERT_NE(pathOffset, std::string::npos) << source;
  ASSERT_NE(svgCloseOffset, std::string::npos) << source;
  EXPECT_LT(pathOffset, svgCloseOffset)
      << "finalized pen path must be spliced inside the root <svg>, not after </svg>:\n"
      << source;
  EXPECT_EQ(source.find("<path", svgCloseOffset), std::string::npos)
      << "no <path> may appear after the root </svg>:\n"
      << source;

  // Re-parse the serialized source and confirm the path is a direct child of
  // the root <svg> — proof it renders, not just that the bytes precede </svg>.
  EditorApp reparsed;
  ASSERT_TRUE(reparsed.loadFromString(source)) << source;
  auto reparsedPath = reparsed.document().document().querySelector("path");
  ASSERT_TRUE(reparsedPath.has_value()) << source;
  const auto reparsedParent = reparsedPath->parentElement();
  ASSERT_TRUE(reparsedParent.has_value()) << source;
  EXPECT_EQ(*reparsedParent, reparsed.document().document().svgElement()) << source;
}

TEST_F(PenToolTest, DragControlPointUsesAlignedCouplingOnSmoothAnchor) {
  // Place a corner anchor, then a smooth anchor via click-drag so the path is
  // "M 10 10 C 10 10 50 -10 50 10" with mirrored handles on (50,10).
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(10.0, 10.0));
  ASSERT_TRUE(app.flushFrame());

  tool.onMouseDown(app, Vector2d(50.0, 10.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(50.0, 30.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(50.0, 30.0));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_EQ(tool.activePathData(), "M 10 10 C 10 10 50 -10 50 10");

  // Grab the smooth anchor's outgoing handle at (50,30) and drag it to
  // (70,10). The incoming handle must rotate to stay collinear while keeping
  // its original length (20).
  tool.onMouseDown(app, Vector2d(50.0, 30.0), MouseModifiers{});
  ASSERT_TRUE(tool.isDraggingAnchor());
  tool.onMouseMove(app, Vector2d(70.0, 10.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(70.0, 10.0));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(path().d(), "M 10 10 C 10 10 30 10 50 10");
}

TEST_F(PenToolTest, OptionDragControlPointBreaksCoupling) {
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(10.0, 10.0));
  ASSERT_TRUE(app.flushFrame());

  tool.onMouseDown(app, Vector2d(50.0, 10.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(50.0, 30.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(50.0, 30.0));
  ASSERT_TRUE(app.flushFrame());

  // Alt/Option-drag the outgoing handle: the incoming handle (50,-10) must
  // stay put while the outgoing handle follows the pointer.
  MouseModifiers option;
  option.option = true;
  tool.onMouseDown(app, Vector2d(50.0, 30.0), option);
  ASSERT_TRUE(tool.isDraggingAnchor());
  tool.onMouseMove(app, Vector2d(70.0, 30.0), /*buttonHeld=*/true, option);
  tool.onMouseUp(app, Vector2d(70.0, 30.0));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(path().d(), "M 10 10 C 10 10 50 -10 50 10");
  EXPECT_EQ(tool.activePathData(), "M 10 10 C 10 10 50 -10 50 10");
}

TEST_F(PenToolTest, ShiftDragControlPointConstrainsAngle) {
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(10.0, 10.0));
  ASSERT_TRUE(app.flushFrame());

  tool.onMouseDown(app, Vector2d(50.0, 10.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(50.0, 30.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(50.0, 30.0));
  ASSERT_TRUE(app.flushFrame());

  // Shift-drag the outgoing handle toward (74,16): nearest 45-degree axis from
  // the anchor (50,10) is horizontal, so the handle projects onto y = 10.
  MouseModifiers shift;
  shift.shift = true;
  tool.onMouseDown(app, Vector2d(50.0, 30.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(74.0, 16.0), /*buttonHeld=*/true, shift);
  tool.onMouseUp(app, Vector2d(74.0, 16.0));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(path().d(), "M 10 10 C 10 10 30 10 50 10");
}

TEST_F(PenToolTest, DragAnchorMovesPointAndHandles) {
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(10.0, 10.0));
  ASSERT_TRUE(app.flushFrame());

  tool.onMouseDown(app, Vector2d(50.0, 10.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(50.0, 30.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(50.0, 30.0));
  ASSERT_TRUE(app.flushFrame());

  // Drag the smooth anchor itself from (50,10) to (60,20): the anchor and both
  // handles translate together.
  tool.onMouseDown(app, Vector2d(50.0, 10.0), MouseModifiers{});
  ASSERT_TRUE(tool.isDraggingAnchor());
  tool.onMouseMove(app, Vector2d(60.0, 20.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(60.0, 20.0));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(path().d(), "M 10 10 C 10 10 60 0 60 20");
}

TEST_F(PenToolTest, ClickLastAnchorRetractsOnlyOutgoingHandle) {
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(10.0, 10.0));
  ASSERT_TRUE(app.flushFrame());

  tool.onMouseDown(app, Vector2d(50.0, 10.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(50.0, 30.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(50.0, 30.0));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_EQ(path().d(), "M 10 10 C 10 10 50 -10 50 10");

  // Click (no drag) on the draft's last anchor retracts only its OUTGOING
  // handle: the segment INTO the anchor keeps its curvature, and the next
  // segment starts straight.
  tool.onMouseDown(app, Vector2d(50.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(50.0, 10.0));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(path().d(), "M 10 10 C 10 10 50 -10 50 10")
      << "the incoming curvature must survive the retract";
  EXPECT_TRUE(tool.isDrafting());

  tool.onMouseDown(app, Vector2d(90.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(90.0, 10.0));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(path().d(), "M 10 10 C 10 10 50 -10 50 10 L 90 10")
      << "the segment after the retracted anchor starts straight";
}

TEST_F(PenToolTest, AltClickSmoothAnchorConvertsToCorner) {
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(10.0, 10.0));
  ASSERT_TRUE(app.flushFrame());

  tool.onMouseDown(app, Vector2d(50.0, 10.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(50.0, 30.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(50.0, 30.0));
  ASSERT_TRUE(app.flushFrame());

  tool.onMouseDown(app, Vector2d(90.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(90.0, 10.0));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_EQ(path().d(), "M 10 10 C 10 10 50 -10 50 10 C 50 30 90 10 90 10");

  // Alt/Option-click (no drag) on the smooth middle anchor converts it to a
  // corner: both handles retract and the adjacent segments straighten.
  MouseModifiers option;
  option.option = true;
  tool.onMouseDown(app, Vector2d(50.0, 10.0), option);
  tool.onMouseUp(app, Vector2d(50.0, 10.0));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(path().d(), "M 10 10 L 50 10 L 90 10");
  EXPECT_TRUE(tool.isDrafting());
}

TEST_F(PenToolTest, AltClickCornerAnchorConvertsToSmooth) {
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(10.0, 10.0));
  ASSERT_TRUE(app.flushFrame());
  tool.onMouseDown(app, Vector2d(50.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(50.0, 10.0));
  ASSERT_TRUE(app.flushFrame());
  tool.onMouseDown(app, Vector2d(90.0, 50.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(90.0, 50.0));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_EQ(path().d(), "M 10 10 L 50 10 L 90 50");

  // Alt/Option-click on the corner middle anchor converts it to a smooth
  // point: mirrored handles along the neighbor chord turn both adjacent
  // segments into curves.
  MouseModifiers option;
  option.option = true;
  tool.onMouseDown(app, Vector2d(50.0, 10.0), option);
  tool.onMouseUp(app, Vector2d(50.0, 10.0));
  ASSERT_TRUE(app.flushFrame());

  const std::string d(std::string_view(path().d()));
  EXPECT_THAT(d, ::testing::StartsWith("M 10 10 C 10 10 "));
  EXPECT_THAT(d, ::testing::Not(::testing::HasSubstr("L")))
      << "both segments adjacent to the smoothed anchor must be curves: " << d;
  EXPECT_TRUE(tool.isDrafting());
}

TEST_F(PenToolTest, ClickDragOnCloseShapesClosingAnchorHandles) {
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(10.0, 10.0));
  ASSERT_TRUE(app.flushFrame());
  tool.onMouseDown(app, Vector2d(70.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(70.0, 10.0));
  ASSERT_TRUE(app.flushFrame());
  tool.onMouseDown(app, Vector2d(40.0, 70.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(40.0, 70.0));
  ASSERT_TRUE(app.flushFrame());

  // Mouse-down on the first anchor closes the contour, and KEEPING the button
  // down while dragging shapes the closing anchor's mirrored handles — the
  // closing segment becomes a cubic (serialized explicitly before the Z) and
  // the first segment curves with the mirrored outgoing handle. The session
  // finalizes on mouse-up.
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(30.0, -10.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(30.0, -10.0));
  ASSERT_TRUE(app.flushFrame());

  const std::string d(std::string_view(path().d()));
  EXPECT_THAT(d, ::testing::EndsWith("Z"));
  EXPECT_THAT(d, ::testing::StartsWith("M 10 10 C 30 -10 "))
      << "the mirrored outgoing handle must curve the first segment: " << d;
  EXPECT_THAT(d, ::testing::HasSubstr("C 40 70 -10 30 10 10 Z"))
      << "the closing segment must serialize as an explicit cubic into the start anchor: " << d;
  EXPECT_FALSE(tool.isDrafting()) << "close-drag finalizes on mouse-up";
}

TEST_F(PenToolTest, ClickOnDraftSegmentInsertsAnchor) {
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(10.0, 10.0));
  ASSERT_TRUE(app.flushFrame());
  tool.onMouseDown(app, Vector2d(90.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(90.0, 10.0));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_EQ(path().d(), "M 10 10 L 90 10");

  // Clicking ON the existing segment (within hit tolerance) inserts an anchor
  // at the hit point instead of appending a new trailing segment.
  tool.onMouseDown(app, Vector2d(50.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(50.0, 10.0));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(path().d(), "M 10 10 L 50 10 L 90 10");
  EXPECT_TRUE(tool.isDrafting());
}

TEST_F(PenToolTest, ClickOnCommittedSelectedPathSegmentInsertsAnchor) {
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <path id="p" d="M 0 0 L 40 0" fill="none" stroke="black"/>
         </svg>)"));
  auto selected = app.document().document().querySelector("#p");
  ASSERT_TRUE(selected.has_value());
  app.setSelection(*selected);

  // Clicking mid-segment (past the anchor hit tolerance) inserts an anchor
  // there as a self-contained point-edit session (one undo, not drafting
  // afterwards) instead of continuing the open path with an appended segment.
  tool.onMouseDown(app, Vector2d(20.0, 0.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(20.0, 0.0));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(selected->cast<svg::SVGPathElement>().d(), "M 0 0 L 20 0 L 40 0");
  EXPECT_FALSE(tool.isDrafting());
}

TEST_F(PenToolTest, EditingCommittedSelectedPathDragsAnchor) {
  ASSERT_TRUE(app.loadFromString(kOpenPathSvg));
  auto selected = app.document().document().querySelector("#p");
  ASSERT_TRUE(selected.has_value());
  app.setSelection(*selected);

  // Drag the interior anchor (0,0)? — drag the first anchor (0,0) of the
  // committed path to (4,4). Editing a committed path is a self-contained
  // session: not drafting afterwards.
  tool.onMouseDown(app, Vector2d(0.0, 0.0), MouseModifiers{});
  ASSERT_TRUE(tool.isDraggingAnchor());
  tool.onMouseMove(app, Vector2d(4.0, 4.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(4.0, 4.0));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(selected->cast<svg::SVGPathElement>().d(), "M 4 4 L 10 0");
  EXPECT_FALSE(tool.isDrafting());
}

TEST_F(PenToolTest, EditingCommittedClosedPathPreservesClosure) {
  constexpr std::string_view kClosedPathSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <path id="c" d="M 10 10 L 40 10 L 40 40 Z" fill="none" stroke="black"/>
         </svg>)";
  ASSERT_TRUE(app.loadFromString(kClosedPathSvg));
  auto selected = app.document().document().querySelector("#c");
  ASSERT_TRUE(selected.has_value());
  app.setSelection(*selected);

  tool.onMouseDown(app, Vector2d(40.0, 40.0), MouseModifiers{});
  ASSERT_TRUE(tool.isDraggingAnchor());
  tool.onMouseMove(app, Vector2d(44.0, 44.0), /*buttonHeld=*/true);
  tool.onMouseUp(app, Vector2d(44.0, 44.0));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(selected->cast<svg::SVGPathElement>().d(), "M 10 10 L 40 10 L 44 44 Z");
  EXPECT_FALSE(tool.isDrafting());
}

TEST_F(PenToolTest, CommandClickNeverPlacesAnchor) {
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(10.0, 10.0));
  ASSERT_TRUE(app.flushFrame());

  // Cmd/Ctrl-click on empty canvas while drafting: no anchor is placed.
  MouseModifiers command;
  command.command = true;
  tool.onMouseDown(app, Vector2d(60.0, 60.0), command);
  tool.onMouseUp(app, Vector2d(60.0, 60.0));
  EXPECT_FALSE(app.flushFrame());

  EXPECT_EQ(tool.activePathData(), "M 10 10");
}

TEST_F(PenToolTest, ClickOpenEndpointResumesDrafting) {
  ASSERT_TRUE(app.loadFromString(kOpenPathSvg));
  auto selected = app.document().document().querySelector("#p");
  ASSERT_TRUE(selected.has_value());
  app.setSelection(*selected);

  // Click (no drag) exactly on the open endpoint (10,0): drafting resumes
  // without appending an anchor.
  tool.onMouseDown(app, Vector2d(10.0, 0.0), MouseModifiers{});
  tool.onMouseUp(app, Vector2d(10.0, 0.0));
  EXPECT_FALSE(app.flushFrame());
  EXPECT_TRUE(tool.isDrafting());

  // The next click appends from that endpoint.
  tool.onMouseDown(app, Vector2d(30.0, 0.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(selected->cast<svg::SVGPathElement>().d(), "M 0 0 L 10 0 L 30 0");
}

TEST_F(PenToolTest, ShiftConstrainsDiagonalSegment) {
  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());

  // (38,32) is closest to the +45-degree diagonal from (10,10); the projected
  // point lands on the diagonal at (35,35).
  MouseModifiers modifiers;
  modifiers.shift = true;
  tool.onMouseDown(app, Vector2d(38.0, 32.0), modifiers);
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(path().d(), "M 10 10 L 35 35");
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
  tool.onMouseUp(app, Vector2d(11.0, 10.0));
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

// =============================================================================
// Faithful live-backend repro for the user-reported "Pen path inserted after
// </svg> so it never renders" GUI bug.
//
// The bare-EditorApp FinalizedPenPathStaysInsideSvgRootSource test above passes,
// yet the user re-confirmed the bug is live in the GUI. The difference is the
// per-frame source-pane mirror + writeback reparse the real editor runs every
// frame (EditorShell), which the bare-app test skips:
//
//   per frame: app.flushFrame()
//              controller.syncParseErrorMarkers(app, textEditor)
//              controller.applyPendingWritebacks(app, selectTool, textEditor)
//              controller.handleTextEdits(app, textEditor, deltaSeconds)
//
// applyPendingWritebacks mirrors the DOM's source deltas into the text-pane
// buffer; handleTextEdits then dispatches any divergence as a writeback reparse
// that swaps the document on the next flush. This harness mirrors that exact
// frame loop through a multi-click pen session + commitOpenPath finalize, then
// asserts the new <path> stays before the root </svg> AND is a renderable child
// of the root <svg>.
// =============================================================================
class PenToolLiveSyncTest : public ::testing::Test {
protected:
  void Load(std::string_view svg) {
    ASSERT_TRUE(app.loadFromString(svg));
    textEditor.setText(svg);
    textEditor.resetTextChanged();
    controller.emplace(std::string(svg));
  }

  // One faithful editor frame: drain the command queue, then run the source-sync
  // controller in the same order EditorShell does. A large deltaSeconds drains
  // the text-change debounce so any queued writeback reparse is dispatched
  // promptly (the GUI reaches the same state after the debounce idle window).
  void Frame() {
    app.flushFrame();
    controller->syncParseErrorMarkers(app, textEditor);
    controller->applyPendingWritebacks(app, selectTool, textEditor);
    controller->handleTextEdits(app, textEditor, /*deltaSeconds=*/1.0f);
  }

  // Run frames until the editor would be idle (bounded so a bug can't spin).
  void Settle() {
    for (int i = 0; i < 8; ++i) {
      Frame();
    }
  }

  svg::RendererBitmap RenderDocument() {
    svg::Renderer renderer;
    renderer.draw(app.document().document());
    return renderer.takeSnapshot();
  }

  EditorApp app;
  PenTool tool;
  TextEditor textEditor;
  SelectTool selectTool;
  std::optional<DocumentSyncController> controller;
};

TEST_F(PenToolLiveSyncTest, FinalizedPenPathRendersThroughLiveSourceSync) {
  Load(R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"></svg>)");

  tool.onMouseDown(app, Vector2d(10.0, 20.0), MouseModifiers{});
  Frame();
  tool.onMouseDown(app, Vector2d(30.0, 40.0), MouseModifiers{});
  Frame();
  tool.onMouseDown(app, Vector2d(60.0, 70.0), MouseModifiers{});
  Frame();

  ASSERT_TRUE(tool.commitOpenPath(app));
  Settle();

  const std::string source(app.document().document().source());
  const std::size_t pathOffset = source.find("<path");
  const std::size_t svgCloseOffset = source.rfind("</svg>");
  ASSERT_NE(pathOffset, std::string::npos)
      << "the finalized pen path vanished from the document source:\n"
      << source;
  ASSERT_NE(svgCloseOffset, std::string::npos) << source;
  EXPECT_LT(pathOffset, svgCloseOffset)
      << "<path> was placed AFTER the root </svg> by the live source-sync path, "
         "so the shape never renders:\n"
      << source;
  EXPECT_EQ(source.find("<path", svgCloseOffset), std::string::npos)
      << "no <path> may appear after the root </svg>:\n"
      << source;

  auto livePath = app.document().document().querySelector("path");
  ASSERT_TRUE(livePath.has_value())
      << "the finalized pen path is missing from the live DOM (never renders):\n"
      << source;
  const auto liveParent = livePath->parentElement();
  ASSERT_TRUE(liveParent.has_value()) << source;
  EXPECT_EQ(*liveParent, app.document().document().svgElement())
      << "the pen path is not a child of the root <svg>, so it never renders:\n"
      << source;

  EXPECT_EQ(textEditor.getText(), source);
}

TEST_F(PenToolLiveSyncTest, FillChangeOnNewPathUpdatesRenderedPixels) {
  Load(R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"></svg>)");
  app.setActiveFill("none");
  app.setActiveStroke("none");

  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  Frame();
  tool.onMouseDown(app, Vector2d(80.0, 10.0), MouseModifiers{});
  Frame();
  tool.onMouseDown(app, Vector2d(10.0, 80.0), MouseModifiers{});
  Frame();

  ASSERT_TRUE(tool.commitOpenPath(app));
  Settle();
  ASSERT_EQ(app.selectedElements().size(), 1u);

  const svg::RendererBitmap beforeFill = RenderDocument();
  ASSERT_FALSE(beforeFill.empty());
  const std::array<std::uint8_t, 4> beforePixel = PixelAt(beforeFill, 24, 24);
  ASSERT_THAT(beforePixel, svg::test::Alpha(::testing::Lt(16)))
      << "The repro starts with an unfilled, unstroked Pen-created triangle; pixel was "
      << testing::PrintToString(beforePixel);

  ASSERT_TRUE(app.setStylePropertyOnSelection("fill", "#ff0000"));
  Settle();

  const std::string source(app.document().document().source());
  EXPECT_NE(source.find("fill: #ff0000"), std::string::npos)
      << "The source pane update must include the selected path fill change:\n"
      << source;

  const svg::RendererBitmap afterFill = RenderDocument();
  ASSERT_FALSE(afterFill.empty());
  const std::array<std::uint8_t, 4> afterPixel = PixelAt(afterFill, 24, 24);
  EXPECT_THAT(afterPixel, svg::test::Rgba(::testing::Gt(200), ::testing::Lt(40), ::testing::Lt(40),
                                          ::testing::Gt(200)))
      << "Changing fill on a freshly-created Pen path must update rendered pixels, not only "
         "source text. Pixel was "
      << testing::PrintToString(afterPixel) << "\nsource:\n"
      << source;
}

TEST_F(PenToolLiveSyncTest, FillChangeOnClosedNewPathUpdatesRenderedPixels) {
  Load(R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"></svg>)");
  app.setActiveFill("none");
  app.setActiveStroke("none");

  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  Frame();
  tool.onMouseDown(app, Vector2d(80.0, 10.0), MouseModifiers{});
  Frame();
  tool.onMouseDown(app, Vector2d(10.0, 80.0), MouseModifiers{});
  Frame();

  MouseModifiers closeModifiers;
  closeModifiers.pixelsPerDocUnit = 1.0;
  tool.onMouseDown(app, Vector2d(10.0, 10.0), closeModifiers);
  tool.onMouseUp(app, Vector2d(10.0, 10.0));
  Settle();
  ASSERT_FALSE(tool.isDrafting());
  ASSERT_EQ(app.selectedElements().size(), 1u);

  const svg::RendererBitmap beforeFill = RenderDocument();
  ASSERT_FALSE(beforeFill.empty());
  const std::array<std::uint8_t, 4> beforePixel = PixelAt(beforeFill, 24, 24);
  ASSERT_THAT(beforePixel, svg::test::Alpha(::testing::Lt(16)))
      << "The repro starts with an unfilled, unstroked closed Pen path; pixel was "
      << testing::PrintToString(beforePixel);

  ASSERT_TRUE(app.setStylePropertyOnSelection("fill", "#ff0000"));
  Settle();

  const std::string source(app.document().document().source());
  EXPECT_NE(source.find("fill: #ff0000"), std::string::npos)
      << "The source pane update must include the selected closed-path fill change:\n"
      << source;

  auto livePath = app.document().document().querySelector("path");
  ASSERT_TRUE(livePath.has_value()) << source;
  const auto liveParent = livePath->parentElement();
  ASSERT_TRUE(liveParent.has_value()) << source;
  EXPECT_EQ(*liveParent, app.document().document().svgElement())
      << "the closed pen path is not a child of the root <svg>, so it never renders:\n"
      << source;

  const svg::RendererBitmap afterFill = RenderDocument();
  ASSERT_FALSE(afterFill.empty());
  const std::array<std::uint8_t, 4> afterPixel = PixelAt(afterFill, 24, 24);
  EXPECT_THAT(afterPixel, svg::test::Rgba(::testing::Gt(200), ::testing::Lt(40), ::testing::Lt(40),
                                          ::testing::Gt(200)))
      << "Changing fill on a closed Pen-created path must update rendered pixels, not only source "
         "text. Pixel was "
      << testing::PrintToString(afterPixel) << "\nsource:\n"
      << source;
}

TEST_F(PenToolLiveSyncTest, FillChangeOnDraftPathUpdatesRenderedPixels) {
  Load(R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"></svg>)");
  app.setActiveFill("none");
  app.setActiveStroke("none");

  tool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  Frame();
  tool.onMouseDown(app, Vector2d(80.0, 10.0), MouseModifiers{});
  Frame();
  tool.onMouseDown(app, Vector2d(10.0, 80.0), MouseModifiers{});
  Settle();
  ASSERT_TRUE(tool.isDrafting());
  ASSERT_EQ(app.selectedElements().size(), 1u);

  const svg::RendererBitmap beforeFill = RenderDocument();
  ASSERT_FALSE(beforeFill.empty());
  const std::array<std::uint8_t, 4> beforePixel = PixelAt(beforeFill, 24, 24);
  ASSERT_THAT(beforePixel, svg::test::Alpha(::testing::Lt(16)))
      << "The repro starts with an unfilled, unstroked Pen draft; pixel was "
      << testing::PrintToString(beforePixel);

  ASSERT_TRUE(app.setStylePropertyOnSelection("fill", "#ff0000"));
  Settle();

  const std::string source(app.document().document().source());
  EXPECT_NE(source.find("fill: #ff0000"), std::string::npos)
      << "The source pane update must include the selected draft path fill change:\n"
      << source;

  const svg::RendererBitmap afterFill = RenderDocument();
  ASSERT_FALSE(afterFill.empty());
  const std::array<std::uint8_t, 4> afterPixel = PixelAt(afterFill, 24, 24);
  EXPECT_THAT(afterPixel, svg::test::Rgba(::testing::Gt(200), ::testing::Lt(40), ::testing::Lt(40),
                                          ::testing::Gt(200)))
      << "Changing fill on a still-drafting Pen path must update rendered pixels, not only source "
         "text. Pixel was "
      << testing::PrintToString(afterPixel) << "\nsource:\n"
      << source;
}

}  // namespace
}  // namespace donner::editor
