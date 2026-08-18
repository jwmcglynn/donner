/**
 * Tests for \ref donner::svg::CursorType enum and its ostream output operator.
 */

#include "donner/svg/core/CursorType.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/core/Cursor.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref CursorType values.
TEST(CursorTypeTest, OstreamOutput) {
  EXPECT_THAT(CursorType::Auto, ToStringIs("auto"));
  EXPECT_THAT(CursorType::Default, ToStringIs("default"));
  EXPECT_THAT(CursorType::None, ToStringIs("none"));
  EXPECT_THAT(CursorType::Pointer, ToStringIs("pointer"));
  EXPECT_THAT(CursorType::Crosshair, ToStringIs("crosshair"));
  EXPECT_THAT(CursorType::Move, ToStringIs("move"));
  EXPECT_THAT(CursorType::Text, ToStringIs("text"));
  EXPECT_THAT(CursorType::Wait, ToStringIs("wait"));
  EXPECT_THAT(CursorType::Help, ToStringIs("help"));
  EXPECT_THAT(CursorType::NotAllowed, ToStringIs("not-allowed"));
  EXPECT_THAT(CursorType::Grab, ToStringIs("grab"));
  EXPECT_THAT(CursorType::Grabbing, ToStringIs("grabbing"));
  EXPECT_THAT(CursorType::NResize, ToStringIs("n-resize"));
  EXPECT_THAT(CursorType::EResize, ToStringIs("e-resize"));
  EXPECT_THAT(CursorType::SResize, ToStringIs("s-resize"));
  EXPECT_THAT(CursorType::WResize, ToStringIs("w-resize"));
  EXPECT_THAT(CursorType::NEResize, ToStringIs("ne-resize"));
  EXPECT_THAT(CursorType::NWResize, ToStringIs("nw-resize"));
  EXPECT_THAT(CursorType::SEResize, ToStringIs("se-resize"));
  EXPECT_THAT(CursorType::SWResize, ToStringIs("sw-resize"));
  EXPECT_THAT(CursorType::ColResize, ToStringIs("col-resize"));
  EXPECT_THAT(CursorType::RowResize, ToStringIs("row-resize"));
  EXPECT_THAT(CursorType::ZoomIn, ToStringIs("zoom-in"));
  EXPECT_THAT(CursorType::ZoomOut, ToStringIs("zoom-out"));
  EXPECT_THAT(CursorType::ContextMenu, ToStringIs("context-menu"));
  EXPECT_THAT(CursorType::Progress, ToStringIs("progress"));
  EXPECT_THAT(CursorType::Cell, ToStringIs("cell"));
  EXPECT_THAT(CursorType::VerticalText, ToStringIs("vertical-text"));
  EXPECT_THAT(CursorType::Alias, ToStringIs("alias"));
  EXPECT_THAT(CursorType::Copy, ToStringIs("copy"));
  EXPECT_THAT(CursorType::NoDrop, ToStringIs("no-drop"));
  EXPECT_THAT(CursorType::AllScroll, ToStringIs("all-scroll"));
  EXPECT_THAT(CursorType::EWResize, ToStringIs("ew-resize"));
  EXPECT_THAT(CursorType::NSResize, ToStringIs("ns-resize"));
  EXPECT_THAT(CursorType::NESWResize, ToStringIs("nesw-resize"));
  EXPECT_THAT(CursorType::NWSEResize, ToStringIs("nwse-resize"));
}

TEST(CursorTest, OstreamOutput) {
  Cursor cursor;
  cursor.images.push_back(CursorImage{RcString("first.cur"), Vector2d(3.0, 4.0)});
  cursor.images.push_back(CursorImage{RcString("second.cur"), std::nullopt});
  cursor.fallback = CursorType::Pointer;

  EXPECT_THAT(cursor, ToStringIs("url(first.cur) 3 4, url(second.cur), pointer"));
}

}  // namespace donner::svg
