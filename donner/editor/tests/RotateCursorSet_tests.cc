#include "donner/editor/RotateCursorSet.h"

#include <cstddef>
#include <optional>

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

constexpr int kCursorSizePx = 32;

std::size_t CountNonTransparentPixels(const RotateCursorImage& image) {
  std::size_t count = 0;
  for (std::size_t i = 3; i < image.rgba.size(); i += 4) {
    if (image.rgba[i] != 0) {
      ++count;
    }
  }
  return count;
}

std::size_t CountOpaqueBlackPixels(const RotateCursorImage& image) {
  std::size_t count = 0;
  for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
    if (image.rgba[i + 3] > 180 && image.rgba[i] < 40 && image.rgba[i + 1] < 40 &&
        image.rgba[i + 2] < 40) {
      ++count;
    }
  }
  return count;
}

std::size_t CountOpaqueWhitePixels(const RotateCursorImage& image) {
  std::size_t count = 0;
  for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
    if (image.rgba[i + 3] > 180 && image.rgba[i] > 220 && image.rgba[i + 1] > 220 &&
        image.rgba[i + 2] > 220) {
      ++count;
    }
  }
  return count;
}

TEST(RotateCursorSetTest, RendersSvgCursorImagesForAllCorners) {
  for (SelectionTransformCorner corner :
       {SelectionTransformCorner::TopLeft, SelectionTransformCorner::TopRight,
        SelectionTransformCorner::BottomRight, SelectionTransformCorner::BottomLeft}) {
    std::optional<RotateCursorImage> image = RenderRotateCursorImage(corner, nullptr);
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->width, 32);
    EXPECT_EQ(image->height, 32);
    EXPECT_EQ(image->rgba.size(), 32u * 32u * 4u);
    EXPECT_GT(CountNonTransparentPixels(*image), 80u);
    EXPECT_LT(CountNonTransparentPixels(*image), 32u * 32u);
  }
}

TEST(RotateCursorSetTest, RotateCursorUsesBlackGlyphWithWhiteOutline) {
  std::optional<RotateCursorImage> image =
      RenderRotateCursorImage(SelectionTransformCorner::TopLeft, nullptr);

  ASSERT_TRUE(image.has_value());
  EXPECT_GT(CountOpaqueBlackPixels(*image), 30u);
  EXPECT_GT(CountOpaqueWhitePixels(*image), 10u);
}

TEST(RotateCursorSetTest, RendersPanCursorImage) {
  for (PanCursorKind kind : {PanCursorKind::OpenHand, PanCursorKind::ClosedHand}) {
    std::optional<RotateCursorImage> image = RenderPanCursorImage(kind, nullptr);
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->width, 32);
    EXPECT_EQ(image->height, 32);
    EXPECT_EQ(image->rgba.size(), 32u * 32u * 4u);
    EXPECT_GT(CountNonTransparentPixels(*image), 100u);
    EXPECT_LT(CountNonTransparentPixels(*image), 32u * 32u);
  }
}

TEST(RotateCursorSetTest, PenCursorUsesBlackGlyphWithWhiteOutline) {
  std::optional<RotateCursorImage> image = RenderPenCursorImage(nullptr);

  ASSERT_TRUE(image.has_value());
  EXPECT_EQ(image->width, 32);
  EXPECT_EQ(image->height, 32);
  EXPECT_EQ(image->rgba.size(), 32u * 32u * 4u);
  EXPECT_GT(CountNonTransparentPixels(*image), 90u);
  EXPECT_LT(CountNonTransparentPixels(*image), 32u * 32u);
  EXPECT_GT(CountOpaqueBlackPixels(*image), 45u);
  EXPECT_GT(CountOpaqueWhitePixels(*image), 8u);
}

TEST(RotateCursorSetTest, OpenAndClosedPanCursorsProduceDifferentBitmaps) {
  std::optional<RotateCursorImage> openHand =
      RenderPanCursorImage(PanCursorKind::OpenHand, nullptr);
  std::optional<RotateCursorImage> closedHand =
      RenderPanCursorImage(PanCursorKind::ClosedHand, nullptr);

  ASSERT_TRUE(openHand.has_value());
  ASSERT_TRUE(closedHand.has_value());
  EXPECT_NE(openHand->rgba, closedHand->rgba);
}

TEST(RotateCursorSetTest, RotatedCornersProduceDifferentBitmaps) {
  std::optional<RotateCursorImage> topLeft =
      RenderRotateCursorImage(SelectionTransformCorner::TopLeft, nullptr);
  std::optional<RotateCursorImage> topRight =
      RenderRotateCursorImage(SelectionTransformCorner::TopRight, nullptr);

  ASSERT_TRUE(topLeft.has_value());
  ASSERT_TRUE(topRight.has_value());
  EXPECT_NE(topLeft->rgba, topRight->rgba);
}

// Cursor-set completeness: every tool state in the `EditorCursor` enum must
// render to a valid image through the shared Donner-render path, and every
// cursor must declare an in-bounds hotspot. This is the "every tool state maps
// to a cursor with a hotspot" guarantee - adding an EditorCursor value without
// wiring its art fails here.
TEST(RotateCursorSetTest, EveryEditorCursorRendersWithAnInBoundsHotspot) {
  for (EditorCursor cursor : kEditorCursors) {
    const SelectionTransformCorner corner = CursorUsesCorner(cursor)
                                                ? SelectionTransformCorner::TopRight
                                                : SelectionTransformCorner::TopLeft;
    std::optional<RotateCursorImage> image = RenderEditorCursorImage(cursor, corner, nullptr);
    ASSERT_TRUE(image.has_value()) << "cursor index " << static_cast<int>(cursor);
    EXPECT_EQ(image->width, kCursorSizePx);
    EXPECT_EQ(image->height, kCursorSizePx);
    EXPECT_EQ(image->rgba.size(), static_cast<std::size_t>(kCursorSizePx) * kCursorSizePx * 4u);
    EXPECT_GT(CountNonTransparentPixels(*image), 40u)
        << "cursor index " << static_cast<int>(cursor);
    EXPECT_LT(CountNonTransparentPixels(*image),
              static_cast<std::size_t>(kCursorSizePx) * kCursorSizePx);
    EXPECT_GT(CountOpaqueBlackPixels(*image), 8u) << "cursor index " << static_cast<int>(cursor);
    EXPECT_GT(CountOpaqueWhitePixels(*image), 4u) << "cursor index " << static_cast<int>(cursor);

    const CursorHotspot hotspot = HotspotForCursor(cursor);
    EXPECT_GE(hotspot.x, 0);
    EXPECT_LT(hotspot.x, kCursorSizePx);
    EXPECT_GE(hotspot.y, 0);
    EXPECT_LT(hotspot.y, kCursorSizePx);
  }
}

// Corner-oriented cursors render for all four corners; fixed cursors do not
// depend on the corner argument.
TEST(RotateCursorSetTest, CornerCursorsRenderForAllCorners) {
  for (EditorCursor cursor : {EditorCursor::Rotate, EditorCursor::Scale}) {
    ASSERT_TRUE(CursorUsesCorner(cursor));
    for (SelectionTransformCorner corner :
         {SelectionTransformCorner::TopLeft, SelectionTransformCorner::TopRight,
          SelectionTransformCorner::BottomRight, SelectionTransformCorner::BottomLeft}) {
      EXPECT_TRUE(RenderEditorCursorImage(cursor, corner, nullptr).has_value());
    }
  }
}

TEST(RotateCursorSetTest, ScaleCursorIsBlackGlyphWithWhiteOutlineAndRotatesPerCorner) {
  std::optional<RotateCursorImage> topLeft =
      RenderScaleCursorImage(SelectionTransformCorner::TopLeft, nullptr);
  std::optional<RotateCursorImage> topRight =
      RenderScaleCursorImage(SelectionTransformCorner::TopRight, nullptr);

  ASSERT_TRUE(topLeft.has_value());
  ASSERT_TRUE(topRight.has_value());
  EXPECT_GT(CountOpaqueBlackPixels(*topLeft), 20u);
  EXPECT_GT(CountOpaqueWhitePixels(*topLeft), 8u);
  EXPECT_NE(topLeft->rgba, topRight->rgba);
}

TEST(RotateCursorSetTest, SelectAndPathModifyRenderDistinctGlyphs) {
  std::optional<RotateCursorImage> select = RenderSelectCursorImage(nullptr);
  std::optional<RotateCursorImage> pathModify = RenderPathModifyCursorImage(nullptr);

  ASSERT_TRUE(select.has_value());
  ASSERT_TRUE(pathModify.has_value());
  EXPECT_GT(CountOpaqueBlackPixels(*select), 20u);
  EXPECT_GT(CountOpaqueBlackPixels(*pathModify), 10u);
  EXPECT_NE(select->rgba, pathModify->rgba);
}

// Each contextual pen hint (add / remove / close) draws a different badge over
// the same base nib, so all four bitmaps must differ.
TEST(RotateCursorSetTest, ContextualPenHintsProduceDistinctBitmaps) {
  std::optional<RotateCursorImage> base = RenderPenCursorImage(PenCursorHint::Base, nullptr);
  std::optional<RotateCursorImage> add = RenderPenCursorImage(PenCursorHint::Add, nullptr);
  std::optional<RotateCursorImage> remove = RenderPenCursorImage(PenCursorHint::Remove, nullptr);
  std::optional<RotateCursorImage> close = RenderPenCursorImage(PenCursorHint::Close, nullptr);

  ASSERT_TRUE(base.has_value());
  ASSERT_TRUE(add.has_value());
  ASSERT_TRUE(remove.has_value());
  ASSERT_TRUE(close.has_value());
  EXPECT_NE(base->rgba, add->rgba);
  EXPECT_NE(base->rgba, remove->rgba);
  EXPECT_NE(base->rgba, close->rgba);
  EXPECT_NE(add->rgba, remove->rgba);
  EXPECT_NE(add->rgba, close->rgba);
  EXPECT_NE(remove->rgba, close->rgba);
  // The badge adds ink to the bottom-right, so a hinted cursor is never fewer
  // pixels than the base nib.
  EXPECT_GE(CountNonTransparentPixels(*add), CountNonTransparentPixels(*base));
}

TEST(RotateCursorSetTest, UninitializedCursorSetRejectsCursorChanges) {
  RotateCursorSet cursorSet;

  EXPECT_FALSE(cursorSet.valid());
  EXPECT_FALSE(cursorSet.setRotateCursor(SelectionTransformCorner::TopLeft));
  EXPECT_FALSE(cursorSet.setRotateCursor(SelectionTransformCorner::BottomRight));
  EXPECT_FALSE(cursorSet.setScaleCursor(SelectionTransformCorner::TopLeft));
  EXPECT_FALSE(cursorSet.setSelectCursor());
  EXPECT_FALSE(cursorSet.setPathModifyCursor());
  EXPECT_FALSE(cursorSet.setPanCursor(PanCursorKind::OpenHand));
  EXPECT_FALSE(cursorSet.setPanCursor(PanCursorKind::ClosedHand));
  EXPECT_FALSE(cursorSet.setPenCursor());
  EXPECT_FALSE(cursorSet.setPenCursor(PenCursorHint::Add));
  EXPECT_FALSE(cursorSet.setPenCursor(PenCursorHint::Remove));
  EXPECT_FALSE(cursorSet.setPenCursor(PenCursorHint::Close));

  cursorSet.clearIfActive();
  EXPECT_FALSE(cursorSet.valid());
}

TEST(RotateCursorSetTest, InitializeWithNullWindowFailsAndLeavesSetInvalid) {
  RotateCursorSet cursorSet;

  EXPECT_FALSE(cursorSet.initialize(nullptr, nullptr));
  EXPECT_FALSE(cursorSet.valid());
  EXPECT_FALSE(cursorSet.setRotateCursor(SelectionTransformCorner::TopRight));
  EXPECT_FALSE(cursorSet.setPanCursor(PanCursorKind::ClosedHand));
  EXPECT_FALSE(cursorSet.setPenCursor());
}

}  // namespace
}  // namespace donner::editor
