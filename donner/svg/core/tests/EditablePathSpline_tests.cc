#include "donner/svg/core/EditablePathSpline.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/core/tests/PathSplineTestUtils.h"

using testing::ElementsAre;
using testing::SizeIs;

namespace donner::svg {

namespace {

using Command = PathSpline::Command;
using CommandType = PathSpline::CommandType;

constexpr Vector2d kP0(0.0, 0.0);
constexpr Vector2d kP1(10.0, 20.0);
constexpr Vector2d kP2(30.0, 40.0);
constexpr Vector2d kC0(5.0, 6.0);
constexpr Vector2d kC1(7.0, 8.0);
constexpr Vector2d kC2(11.0, 12.0);
constexpr Vector2d kC3(13.0, 14.0);

}  // namespace

TEST(EditablePathSpline, ConvertsOpenPolylineToSingleContour) {
  PathSpline spline;
  spline.moveTo(kP0);
  spline.lineTo(kP1);
  spline.lineTo(kP2);

  const EditablePathSpline editable = EditablePathSpline::FromPathSpline(spline);

  ASSERT_THAT(editable.contours(), SizeIs(1));
  EXPECT_FALSE(editable.contours()[0].closed);
  ASSERT_THAT(editable.contours()[0].anchors, SizeIs(3));
  EXPECT_EQ(editable.contours()[0].anchors[0].position, kP0);
  EXPECT_EQ(editable.contours()[0].anchors[1].position, kP1);
  EXPECT_EQ(editable.contours()[0].anchors[2].position, kP2);
  EXPECT_EQ(editable.contours()[0].anchors[1].mode, PathAnchorMode::Corner);
}

TEST(EditablePathSpline, ConvertsCubicSegmentHandles) {
  PathSpline spline;
  spline.moveTo(kP0);
  spline.curveTo(kC0, kC1, kP1);
  spline.curveTo(kC2, kC3, kP2);

  const EditablePathSpline editable = EditablePathSpline::FromPathSpline(spline);

  ASSERT_THAT(editable.contours(), SizeIs(1));
  const auto& contour = editable.contours()[0];
  ASSERT_THAT(contour.anchors, SizeIs(3));

  EXPECT_EQ(contour.anchors[0].outgoingHandle, kC0);
  EXPECT_EQ(contour.anchors[1].incomingHandle, kC1);
  EXPECT_EQ(contour.anchors[1].outgoingHandle, kC2);
  EXPECT_EQ(contour.anchors[2].incomingHandle, kC3);
}

TEST(EditablePathSpline, RoundTripsClosedContour) {
  PathSpline spline;
  spline.moveTo(kP0);
  spline.lineTo(kP1);
  spline.lineTo(kP2);
  spline.closePath();

  const EditablePathSpline editable = EditablePathSpline::FromPathSpline(spline);
  const PathSpline roundTripped = editable.toPathSpline();

  EXPECT_THAT(roundTripped, PointsAndCommandsAre(
                                ElementsAre(kP0, kP1, kP2),
                                ElementsAre(Command(CommandType::MoveTo, 0),
                                            Command(CommandType::LineTo, 1),
                                            Command(CommandType::LineTo, 2),
                                            Command(CommandType::ClosePath, 0))));
}

TEST(EditablePathSpline, RoundTripsMultipleContours) {
  PathSpline spline;
  spline.moveTo(kP0);
  spline.lineTo(kP1);
  spline.moveTo(kP2);
  spline.curveTo(kC0, kC1, kP0);

  const EditablePathSpline editable = EditablePathSpline::FromPathSpline(spline);
  const PathSpline roundTripped = editable.toPathSpline();

  EXPECT_THAT(roundTripped, PointsAndCommandsAre(
                                ElementsAre(kP0, kP1, kP2, kC0, kC1, kP0),
                                ElementsAre(Command(CommandType::MoveTo, 0),
                                            Command(CommandType::LineTo, 1),
                                            Command(CommandType::MoveTo, 2),
                                            Command(CommandType::CurveTo, 3))));
}

TEST(EditablePathSpline, MoveAnchorPreservesHandleOffsets) {
  PathSpline spline;
  spline.moveTo(kP0);
  spline.curveTo(kC0, kC1, kP1);

  EditablePathSpline editable = EditablePathSpline::FromPathSpline(spline);
  auto& anchor = editable.contours()[0].anchors[1];
  const PathAnchorId anchorId = anchor.id;
  const Vector2d originalIncoming = *anchor.incomingHandle;

  editable.moveAnchor(anchorId, Vector2d(20.0, 30.0));

  const EditablePathAnchor* movedAnchor = editable.findAnchor(anchorId);
  ASSERT_NE(movedAnchor, nullptr);
  EXPECT_EQ(movedAnchor->position, Vector2d(20.0, 30.0));
  EXPECT_EQ(*movedAnchor->incomingHandle, originalIncoming + Vector2d(10.0, 10.0));
}

TEST(EditablePathSpline, SymmetricModeMirrorsOppositeHandle) {
  EditablePathSpline editable;
  const PathContourId contourId = editable.addContour();
  const PathAnchorId anchorId =
      editable.appendAnchor(contourId, Vector2d(10.0, 10.0), Vector2d(5.0, 10.0), Vector2d(15.0, 10.0));

  editable.setAnchorMode(anchorId, PathAnchorMode::Symmetric);
  editable.moveHandle(anchorId, PathHandleType::Outgoing, Vector2d(18.0, 12.0));

  const EditablePathAnchor* anchor = editable.findAnchor(anchorId);
  ASSERT_NE(anchor, nullptr);
  ASSERT_TRUE(anchor->incomingHandle.has_value());
  ASSERT_TRUE(anchor->outgoingHandle.has_value());
  EXPECT_EQ(anchor->mode, PathAnchorMode::Symmetric);
  EXPECT_EQ(*anchor->incomingHandle, Vector2d(2.0, 8.0));
  EXPECT_EQ(*anchor->outgoingHandle, Vector2d(18.0, 12.0));
}

TEST(EditablePathSpline, SetAnchorHandlesAppliesExplicitMode) {
  EditablePathSpline editable;
  const PathContourId contourId = editable.addContour();
  const PathAnchorId anchorId = editable.appendAnchor(contourId, Vector2d(10.0, 10.0));

  editable.setAnchorHandles(anchorId, Vector2d(6.0, 10.0), Vector2d(16.0, 10.0),
                            PathAnchorMode::Symmetric);

  const EditablePathAnchor* anchor = editable.findAnchor(anchorId);
  ASSERT_NE(anchor, nullptr);
  ASSERT_TRUE(anchor->incomingHandle.has_value());
  ASSERT_TRUE(anchor->outgoingHandle.has_value());
  EXPECT_EQ(anchor->mode, PathAnchorMode::Symmetric);
  EXPECT_EQ(*anchor->incomingHandle, Vector2d(4.0, 10.0));
  EXPECT_EQ(*anchor->outgoingHandle, Vector2d(16.0, 10.0));
}

}  // namespace donner::svg
