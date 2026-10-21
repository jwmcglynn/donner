#include "donner/editor/SidebarPresenter.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/MathUtils.h"
#include "donner/base/Transform.h"
#include "donner/editor/EmbeddedSvgIcon.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/ImGuiInternalIncludes.h"
#include "donner/svg/DocumentState.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::editor {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::Pair;

constexpr std::string_view kInspectorSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
         <rect x="10" y="20" width="100" height="50" fill="red" id="target"/>
         <rect id="peer" x="0" y="0" width="10" height="10"/>
       </svg>)";

/// Matches a box that shares no area with \p other.
MATCHER_P(DoesNotOverlap, other, "shares no area with " + ::testing::PrintToString(other)) {
  const bool overlaps = arg.topLeft.x < other.bottomRight.x &&
                        other.topLeft.x < arg.bottomRight.x &&
                        arg.topLeft.y < other.bottomRight.y && other.topLeft.y < arg.bottomRight.y;
  return !overlaps;
}

/// Screen-space bounds of the open popup drawn in the last frame, if any.
std::optional<Box2d> OpenPopupWindowRect() {
  for (const ImGuiPopupData& popup : ImGui::GetCurrentContext()->OpenPopupStack) {
    if (popup.Window != nullptr && popup.Window->Active && !popup.Window->Hidden) {
      return Box2d(Vector2d(popup.Window->Pos.x, popup.Window->Pos.y),
                   Vector2d(popup.Window->Pos.x + popup.Window->Size.x,
                            popup.Window->Pos.y + popup.Window->Size.y));
    }
  }
  return std::nullopt;
}

ImVec2 RectCenter(const Box2d& rect) {
  return ImVec2(static_cast<float>((rect.topLeft.x + rect.bottomRight.x) * 0.5),
                static_cast<float>((rect.topLeft.y + rect.bottomRight.y) * 0.5));
}

const std::string* FindInspectorValue(std::span<const std::pair<std::string, std::string>> entries,
                                      std::string_view name) {
  for (const auto& [entryName, entryValue] : entries) {
    if (entryName == name) {
      return &entryValue;
    }
  }

  return nullptr;
}

/// Builds a transform from its six raw matrix components (a b c d e f).
Transform2d MakeMatrix(double a, double b, double c, double d, double e, double f) {
  Transform2d result(Transform2d::uninitialized);
  result.data[0] = a;
  result.data[1] = b;
  result.data[2] = c;
  result.data[3] = d;
  result.data[4] = e;
  result.data[5] = f;
  return result;
}

void ExpectTransformNear(const Transform2d& actual, const Transform2d& expected, double tolerance) {
  for (int i = 0; i < 6; ++i) {
    EXPECT_NEAR(actual.data[i], expected.data[i], tolerance) << "matrix component " << i;
  }
}

TEST(SidebarTransformDecomposeTest, IdentityRoundTrips) {
  const std::optional<DecomposedTransform> decomposed = DecomposeTransform(Transform2d());
  ASSERT_TRUE(decomposed.has_value());
  EXPECT_DOUBLE_EQ(decomposed->translation.x, 0.0);
  EXPECT_DOUBLE_EQ(decomposed->translation.y, 0.0);
  EXPECT_DOUBLE_EQ(decomposed->rotationRadians, 0.0);
  EXPECT_DOUBLE_EQ(decomposed->scale.x, 1.0);
  EXPECT_DOUBLE_EQ(decomposed->scale.y, 1.0);
  ExpectTransformNear(ComposeTransform(*decomposed), Transform2d(), 0.0);
}

TEST(SidebarTransformDecomposeTest, TranslateRotateScaleRoundTrips) {
  const DecomposedTransform fields{
      .translation = Vector2d(12.5, -3.75),
      .rotationRadians = 30.0 * MathConstants<double>::kDegToRad,
      .scale = Vector2d(2.0, 0.5),
  };
  const Transform2d matrix = ComposeTransform(fields);

  const std::optional<DecomposedTransform> roundTrip = DecomposeTransform(matrix);
  ASSERT_TRUE(roundTrip.has_value());
  EXPECT_NEAR(roundTrip->translation.x, fields.translation.x, 1e-12);
  EXPECT_NEAR(roundTrip->translation.y, fields.translation.y, 1e-12);
  EXPECT_NEAR(roundTrip->rotationRadians, fields.rotationRadians, 1e-12);
  EXPECT_NEAR(roundTrip->scale.x, fields.scale.x, 1e-12);
  EXPECT_NEAR(roundTrip->scale.y, fields.scale.y, 1e-12);

  ExpectTransformNear(ComposeTransform(*roundTrip), matrix, 1e-12);
}

TEST(SidebarTransformDecomposeTest, PureRotationDecomposes) {
  const Transform2d matrix = Transform2d::Rotate(1.0);
  const std::optional<DecomposedTransform> decomposed = DecomposeTransform(matrix);
  ASSERT_TRUE(decomposed.has_value());
  EXPECT_NEAR(decomposed->rotationRadians, 1.0, 1e-12);
  EXPECT_NEAR(decomposed->scale.x, 1.0, 1e-12);
  EXPECT_NEAR(decomposed->scale.y, 1.0, 1e-12);
  ExpectTransformNear(ComposeTransform(*decomposed), matrix, 1e-15);
}

TEST(SidebarTransformDecomposeTest, FlipRoundTripsThroughSignedScale) {
  // A horizontal flip decomposes as a negative y scale plus a half-turn; the
  // fields differ from the authored scale(-2, 3) but the matrix round-trips.
  const Transform2d matrix = Transform2d::Scale(-2.0, 3.0);
  const std::optional<DecomposedTransform> decomposed = DecomposeTransform(matrix);
  ASSERT_TRUE(decomposed.has_value());
  EXPECT_LT(decomposed->scale.y, 0.0);
  ExpectTransformNear(ComposeTransform(*decomposed), matrix, 1e-12);
}

TEST(SidebarTransformDecomposeTest, LargeTranslationIsExact) {
  const Transform2d matrix = Transform2d::Rotate(0.25) * Transform2d::Translate(1.0e6, -2.0e6);
  const std::optional<DecomposedTransform> decomposed = DecomposeTransform(matrix);
  ASSERT_TRUE(decomposed.has_value());
  // Translation components pass through decompose/compose verbatim.
  EXPECT_EQ(ComposeTransform(*decomposed).data[4], matrix.data[4]);
  EXPECT_EQ(ComposeTransform(*decomposed).data[5], matrix.data[5]);
}

TEST(SidebarTransformDecomposeTest, SkewReturnsNullopt) {
  EXPECT_FALSE(DecomposeTransform(Transform2d::SkewX(0.3)).has_value());
  EXPECT_FALSE(DecomposeTransform(Transform2d::SkewY(-0.2)).has_value());
  // Rotation times skew is not orthogonal either.
  EXPECT_FALSE(DecomposeTransform(Transform2d::SkewX(0.3) * Transform2d::Rotate(0.5)).has_value());
}

TEST(SidebarTransformDecomposeTest, SingularReturnsNullopt) {
  EXPECT_FALSE(DecomposeTransform(MakeMatrix(0, 0, 0, 0, 10, 20)).has_value());
  EXPECT_FALSE(DecomposeTransform(Transform2d::Scale(0.0, 5.0)).has_value());
}

TEST(SidebarTransformDecomposeTest, CollapsedYAxisStillRoundTrips) {
  // scale(3, 0) zeroes the y basis column but keeps a valid x basis; it
  // decomposes to a zero y scale and composes back exactly.
  const Transform2d matrix = Transform2d::Scale(3.0, 0.0);
  const std::optional<DecomposedTransform> decomposed = DecomposeTransform(matrix);
  ASSERT_TRUE(decomposed.has_value());
  EXPECT_DOUBLE_EQ(decomposed->scale.y, 0.0);
  ExpectTransformNear(ComposeTransform(*decomposed), matrix, 1e-15);
}

// -----------------------------------------------------------------------------
// Decomposed-edit round trips: drive the transform-edit state machine the way
// the DragFloat widgets do (begin -> apply -> commit), flush the queued
// mutation, and verify the refreshed snapshot re-derives the edited field.

constexpr std::string_view kRotatedEditSvg =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
         <rect id="target" x="10" y="20" width="40" height="30" transform="rotate(30)"/>
       </svg>)SVG";

/// Loads @p source, selects `#target`, and refreshes @p presenter's snapshot.
void LoadAndSelectTarget(EditorApp& app, SidebarPresenter& presenter, std::string_view source) {
  ASSERT_TRUE(app.loadFromString(source));
  app.setCleanSourceText(source);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);
  presenter.refreshSnapshot(app);
  ASSERT_TRUE(presenter.inspectorHasSelectionForTesting());
  ASSERT_TRUE(presenter.inspectorBoundsForTesting().has_value());
}

/// Runs one complete edit on @p field: begin, write @p value, commit, flush
/// the queued SetTransformCommand, and refresh the snapshot.
void EditTransformField(EditorApp& app, SidebarPresenter& presenter,
                        SidebarPresenter::TransformField field, double value, int matrixIndex = 0) {
  presenter.beginTransformEditForTesting(app, field, matrixIndex);
  ASSERT_TRUE(presenter.hasTransformEditForTesting());
  EXPECT_TRUE(presenter.applyTransformEditForTesting(app, value));
  presenter.commitTransformEditForTesting(app);
  EXPECT_FALSE(presenter.hasTransformEditForTesting());
  EXPECT_TRUE(app.flushFrame());
  presenter.refreshSnapshot(app);
}

TEST(SidebarTransformEditTest, PositionXEditRoundTripsThroughSnapshot) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kInspectorSvg));
  EXPECT_NEAR(presenter.inspectorBoundsForTesting()->topLeft.x, 10.0, 1e-9);

  ASSERT_NO_FATAL_FAILURE(
      EditTransformField(app, presenter, SidebarPresenter::TransformField::PositionX, 35.0));

  const std::optional<Box2d>& bounds = presenter.inspectorBoundsForTesting();
  ASSERT_TRUE(bounds.has_value());
  EXPECT_NEAR(bounds->topLeft.x, 35.0, 1e-6);
  EXPECT_NEAR(bounds->topLeft.y, 20.0, 1e-6);
  EXPECT_NEAR(bounds->width(), 100.0, 1e-6);
  EXPECT_NEAR(bounds->height(), 50.0, 1e-6);
  EXPECT_TRUE(app.canUndo()) << "a completed edit records exactly one undo entry";
}

TEST(SidebarTransformEditTest, ConcurrentDomActivationDoesNotReenterDocumentLock) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kInspectorSvg));
  app.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);

  presenter.beginTransformEditForTesting(app, SidebarPresenter::TransformField::PositionX);

  EXPECT_TRUE(presenter.hasTransformEditForTesting());
  presenter.commitTransformEditForTesting(app);
}

TEST(SidebarTransformEditTest, WidthEditScalesAboutTopLeftAndRederives) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kInspectorSvg));

  ASSERT_NO_FATAL_FAILURE(
      EditTransformField(app, presenter, SidebarPresenter::TransformField::Width, 150.0));

  const std::optional<Box2d>& bounds = presenter.inspectorBoundsForTesting();
  ASSERT_TRUE(bounds.has_value());
  EXPECT_NEAR(bounds->width(), 150.0, 1e-6);
  EXPECT_NEAR(bounds->topLeft.x, 10.0, 1e-6) << "width scales about the bounds top-left";
  EXPECT_NEAR(bounds->topLeft.y, 20.0, 1e-6);
  EXPECT_NEAR(bounds->height(), 50.0, 1e-6);
}

TEST(SidebarTransformEditTest, RotationEditOnRotatedElementRederivesAngle) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kRotatedEditSvg));

  const std::optional<Transform2d>& startTransform = presenter.inspectorTransformForTesting();
  ASSERT_TRUE(startTransform.has_value());
  const std::optional<DecomposedTransform> startDecomposed = DecomposeTransform(*startTransform);
  ASSERT_TRUE(startDecomposed.has_value());
  EXPECT_NEAR(startDecomposed->rotationRadians * MathConstants<double>::kRadToDeg, 30.0, 1e-6);
  const Box2d startBounds = *presenter.inspectorBoundsForTesting();
  const Vector2d startCenter = (startBounds.topLeft + startBounds.bottomRight) * 0.5;

  ASSERT_NO_FATAL_FAILURE(
      EditTransformField(app, presenter, SidebarPresenter::TransformField::Rotation, 45.0));

  const std::optional<Transform2d>& endTransform = presenter.inspectorTransformForTesting();
  ASSERT_TRUE(endTransform.has_value());
  const std::optional<DecomposedTransform> endDecomposed = DecomposeTransform(*endTransform);
  ASSERT_TRUE(endDecomposed.has_value()) << "rotating keeps the matrix decomposable";
  EXPECT_NEAR(endDecomposed->rotationRadians * MathConstants<double>::kRadToDeg, 45.0, 1e-6)
      << "the rotation field re-derives the edited angle";
  EXPECT_NEAR(endDecomposed->scale.x, startDecomposed->scale.x, 1e-9);
  EXPECT_NEAR(endDecomposed->scale.y, startDecomposed->scale.y, 1e-9);

  // Rotation pivots on the bounds center, so the center stays put.
  const Box2d endBounds = *presenter.inspectorBoundsForTesting();
  const Vector2d endCenter = (endBounds.topLeft + endBounds.bottomRight) * 0.5;
  EXPECT_NEAR(endCenter.x, startCenter.x, 1e-6);
  EXPECT_NEAR(endCenter.y, startCenter.y, 1e-6);
}

TEST(SidebarTransformEditTest, PositionEditOnRotatedElementMovesBoundsAndKeepsRotation) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kRotatedEditSvg));

  const Box2d startBounds = *presenter.inspectorBoundsForTesting();
  const double newX = startBounds.topLeft.x + 15.0;
  ASSERT_NO_FATAL_FAILURE(
      EditTransformField(app, presenter, SidebarPresenter::TransformField::PositionX, newX));

  const std::optional<Box2d>& bounds = presenter.inspectorBoundsForTesting();
  ASSERT_TRUE(bounds.has_value());
  EXPECT_NEAR(bounds->topLeft.x, newX, 1e-6) << "X re-derives the edited document-space value";
  EXPECT_NEAR(bounds->topLeft.y, startBounds.topLeft.y, 1e-6);
  EXPECT_NEAR(bounds->width(), startBounds.width(), 1e-6);
  EXPECT_NEAR(bounds->height(), startBounds.height(), 1e-6);

  const std::optional<DecomposedTransform> decomposed =
      DecomposeTransform(*presenter.inspectorTransformForTesting());
  ASSERT_TRUE(decomposed.has_value());
  EXPECT_NEAR(decomposed->rotationRadians * MathConstants<double>::kRadToDeg, 30.0, 1e-6)
      << "translating must not disturb the rotation field";
}

TEST(SidebarTransformEditTest, WidthEditOnRotatedElementResizesBoundsButIntroducesSkew) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kRotatedEditSvg));

  const Box2d startBounds = *presenter.inspectorBoundsForTesting();
  const double newWidth = startBounds.width() * 2.0;
  ASSERT_NO_FATAL_FAILURE(
      EditTransformField(app, presenter, SidebarPresenter::TransformField::Width, newWidth));

  const std::optional<Box2d>& bounds = presenter.inspectorBoundsForTesting();
  ASSERT_TRUE(bounds.has_value());
  EXPECT_NEAR(bounds->width(), newWidth, 1e-6);
  EXPECT_NEAR(bounds->topLeft.x, startBounds.topLeft.x, 1e-6);

  // Scaling document-space X across a rotated matrix shears it; the scalar
  // fields fall back to disabled and the raw matrix stays the editing route.
  EXPECT_FALSE(DecomposeTransform(*presenter.inspectorTransformForTesting()).has_value());
}

TEST(SidebarTransformEditTest, MatrixCellEditWritesRawComponent) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kInspectorSvg));

  ASSERT_NO_FATAL_FAILURE(EditTransformField(
      app, presenter, SidebarPresenter::TransformField::Matrix, 99.0, /*matrixIndex=*/4));

  const std::optional<Transform2d>& transform = presenter.inspectorTransformForTesting();
  ASSERT_TRUE(transform.has_value());
  EXPECT_DOUBLE_EQ(transform->data[4], 99.0);
  // Untouched cells keep identity.
  EXPECT_DOUBLE_EQ(transform->data[0], 1.0);
  EXPECT_DOUBLE_EQ(transform->data[3], 1.0);
}

TEST(SidebarTransformEditTest, UndoRestoresPreEditTransform) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kRotatedEditSvg));
  const Box2d startBounds = *presenter.inspectorBoundsForTesting();
  const Transform2d startTransform = *presenter.inspectorTransformForTesting();

  ASSERT_NO_FATAL_FAILURE(
      EditTransformField(app, presenter, SidebarPresenter::TransformField::Rotation, 60.0));
  ASSERT_TRUE(app.canUndo());

  app.undo();
  app.flushFrame();
  presenter.refreshSnapshot(app);

  ASSERT_TRUE(presenter.inspectorTransformForTesting().has_value());
  ExpectTransformNear(*presenter.inspectorTransformForTesting(), startTransform, 1e-9);
  const std::optional<Box2d>& bounds = presenter.inspectorBoundsForTesting();
  ASSERT_TRUE(bounds.has_value());
  EXPECT_NEAR(bounds->topLeft.x, startBounds.topLeft.x, 1e-6);
  EXPECT_NEAR(bounds->topLeft.y, startBounds.topLeft.y, 1e-6);
}

TEST(SidebarPresenterTest, RefreshSnapshotCapturesXmlAttributesAndComputedStyle) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);

  auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  EXPECT_TRUE(presenter.inspectorHasSelectionForTesting());

  const auto xmlAttributes = presenter.inspectorXmlAttributesForTesting();
  EXPECT_THAT(xmlAttributes,
              ElementsAre(Pair("x", "10"), Pair("y", "20"), Pair("width", "100"),
                          Pair("height", "50"), Pair("fill", "red"), Pair("id", "target")));

  const auto computedStyle = presenter.inspectorComputedStyleForTesting();
  EXPECT_THAT(computedStyle,
              ElementsAre(Pair("display", ::testing::_), Pair("visibility", ::testing::_),
                          Pair("opacity", ::testing::_), Pair("fill", ::testing::_),
                          Pair("fill-opacity", ::testing::_), Pair("stroke", ::testing::_),
                          Pair("stroke-width", ::testing::_), Pair("stroke-opacity", ::testing::_),
                          Pair("color", ::testing::_)));

  const std::string* displayValue = FindInspectorValue(computedStyle, "display");
  ASSERT_NE(displayValue, nullptr);
  EXPECT_EQ(*displayValue, "inline (default)");

  const std::string* fillValue = FindInspectorValue(computedStyle, "fill");
  ASSERT_NE(fillValue, nullptr);
  EXPECT_EQ(*fillValue, "PaintServer(solid rgba(255, 0, 0, 255)) (set)");

  const std::string* strokeValue = FindInspectorValue(computedStyle, "stroke");
  ASSERT_NE(strokeValue, nullptr);
  EXPECT_EQ(*strokeValue, "PaintServer(none) (default)");

  const std::string* colorValue = FindInspectorValue(computedStyle, "color");
  ASSERT_NE(colorValue, nullptr);
  EXPECT_EQ(*colorValue, "rgba(0, 0, 0, 255) (default)");

  const auto swatches = presenter.inspectorComputedStyleSwatchesForTesting();
  ASSERT_EQ(swatches.size(), computedStyle.size());
  EXPECT_EQ(swatches[3], std::optional<ImU32>(IM_COL32(255, 0, 0, 255)));
  EXPECT_EQ(swatches[5], std::nullopt);
  EXPECT_EQ(swatches[8], std::optional<ImU32>(IM_COL32(0, 0, 0, 255)));
}

TEST(SidebarPresenterTest, FormatsComputedStyleValuesAsCssWithSeparateProvenance) {
  const InspectorStyleDisplayValue solid =
      FormatInspectorStyleValue("PaintServer(solid rgba(255, 0, 0, 255)) (set)");
  EXPECT_EQ(solid.value, "rgba(255, 0, 0, 255)");
  EXPECT_EQ(solid.state, InspectorStyleState::Set);

  const InspectorStyleDisplayValue reference =
      FormatInspectorStyleValue("PaintServer(url(#gradient)) (set)");
  EXPECT_EQ(reference.value, "url(#gradient)");
  EXPECT_EQ(reference.state, InspectorStyleState::Set);

  const InspectorStyleDisplayValue defaultPaint =
      FormatInspectorStyleValue("PaintServer(none) (default)");
  EXPECT_EQ(defaultPaint.value, "none");
  EXPECT_EQ(defaultPaint.state, InspectorStyleState::Default);

  const InspectorStyleDisplayValue unset = FormatInspectorStyleValue("nullopt");
  EXPECT_EQ(unset.value, "not set");
  EXPECT_EQ(unset.state, InspectorStyleState::Unspecified);
}

TEST(SidebarPresenterTest, RefreshSnapshotKeepsAttributesWhenCleanSourceTextIsMissing) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));

  auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  EXPECT_TRUE(presenter.inspectorHasSelectionForTesting());
  const auto xmlAttributes = presenter.inspectorXmlAttributesForTesting();
  EXPECT_THAT(xmlAttributes,
              testing::Contains(testing::Pair(std::string("id"), std::string("target"))));
  EXPECT_THAT(xmlAttributes,
              testing::Contains(testing::Pair(std::string("fill"), std::string("red"))));
  EXPECT_THAT(xmlAttributes,
              testing::Contains(testing::Pair(std::string("width"), std::string("100"))));
}

TEST(SidebarPresenterTest, RefreshSnapshotClearsStateWhenNoDocumentLoaded) {
  EditorApp app;

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  EXPECT_FALSE(presenter.hasTreeSnapshotForTesting());
  EXPECT_FALSE(presenter.inspectorHasSelectionForTesting());
  EXPECT_TRUE(presenter.inspectorTitleForTesting().empty());
  EXPECT_TRUE(presenter.inspectorXmlAttributesForTesting().empty());
  EXPECT_TRUE(presenter.inspectorComputedStyleForTesting().empty());
}

TEST(SidebarPresenterTest, RefreshSnapshotCapturesTitleWithoutIdAndPrefixedAttributes) {
  constexpr std::string_view kImageSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"
              xmlns:xlink="http://www.w3.org/1999/xlink" width="120" height="80">
           <image xlink:href="texture.png" width="80" height="60"/>
         </svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kImageSvg));
  app.setCleanSourceText(kImageSvg);

  auto image = app.document().document().querySelector("image");
  ASSERT_TRUE(image.has_value());
  app.setSelection(*image);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  EXPECT_TRUE(presenter.hasTreeSnapshotForTesting());
  EXPECT_TRUE(presenter.inspectorHasSelectionForTesting());
  EXPECT_EQ(presenter.inspectorTitleForTesting(), "Selected: <image>");

  const auto xmlAttributes = presenter.inspectorXmlAttributesForTesting();
  EXPECT_THAT(xmlAttributes, testing::Contains(testing::Pair(std::string("xlink:href"),
                                                             std::string("texture.png"))));
  EXPECT_THAT(xmlAttributes,
              testing::Contains(testing::Pair(std::string("width"), std::string("80"))));
  EXPECT_THAT(xmlAttributes,
              testing::Contains(testing::Pair(std::string("height"), std::string("60"))));
}

TEST(SidebarPresenterTest, RefreshSnapshotAllowsConcurrentDom) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);
  app.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);

  auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  EXPECT_TRUE(presenter.inspectorHasSelectionForTesting());
  EXPECT_FALSE(presenter.inspectorXmlAttributesForTesting().empty());
  EXPECT_FALSE(presenter.inspectorComputedStyleForTesting().empty());
}

TEST(SidebarPresenterTest, RefreshSnapshotOmitsInspectorDetailsForMultiSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);

  auto target = app.document().document().querySelector("#target");
  auto peer = app.document().document().querySelector("#peer");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(peer.has_value());

  app.setSelection(std::vector<svg::SVGElement>{*target, *peer});

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  EXPECT_FALSE(presenter.inspectorHasSelectionForTesting());
  EXPECT_THAT(presenter.inspectorXmlAttributesForTesting(), IsEmpty());
  EXPECT_THAT(presenter.inspectorComputedStyleForTesting(), IsEmpty());
}

TEST(SidebarTransformEditTest, PositionYEditPreservesTheOtherBoundsCoordinates) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kInspectorSvg));
  ASSERT_NO_FATAL_FAILURE(
      EditTransformField(app, presenter, SidebarPresenter::TransformField::PositionY, 45.0));
  EXPECT_THAT(presenter.inspectorBoundsForTesting(),
              testing::Optional(testing::Eq(Box2d::FromXYWH(10, 45, 100, 50))));
  EXPECT_THAT(app.undoTimeline().entryCount(), testing::Eq(1u));
}

TEST(SidebarTransformEditTest, HeightEditKeepsTheTopLeftAndRoundTripsThroughUndoRedo) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kInspectorSvg));
  ASSERT_NO_FATAL_FAILURE(
      EditTransformField(app, presenter, SidebarPresenter::TransformField::Height, 75.0));
  EXPECT_THAT(presenter.inspectorBoundsForTesting(),
              testing::Optional(testing::Eq(Box2d::FromXYWH(10, 20, 100, 75))));
  EXPECT_THAT(app.undoTimeline().entryCount(), testing::Eq(1u));
  app.undo();
  ASSERT_THAT(app.flushFrame(), testing::IsTrue());
  presenter.refreshSnapshot(app);
  EXPECT_THAT(presenter.inspectorBoundsForTesting(),
              testing::Optional(testing::Eq(Box2d::FromXYWH(10, 20, 100, 50))));
  app.redo();
  ASSERT_THAT(app.flushFrame(), testing::IsTrue());
  presenter.refreshSnapshot(app);
  EXPECT_THAT(presenter.inspectorBoundsForTesting(),
              testing::Optional(testing::Eq(Box2d::FromXYWH(10, 20, 100, 75))));
}

TEST(SidebarTransformEditTest, DegenerateAndNonFiniteSizesLeaveTheDocumentAndHistoryUntouched) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kInspectorSvg));
  const std::string originalSource(app.document().document().source());
  for (const auto field :
       {SidebarPresenter::TransformField::Width, SidebarPresenter::TransformField::Height}) {
    SCOPED_TRACE(static_cast<unsigned>(field));
    presenter.beginTransformEditForTesting(app, field);
    ASSERT_THAT(presenter.hasTransformEditForTesting(), testing::IsTrue());
    for (double value :
         {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
          std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
      SCOPED_TRACE(value);
      EXPECT_THAT(presenter.applyTransformEditForTesting(app, value), testing::IsFalse());
      EXPECT_THAT(app.document().queue().size(), testing::Eq(0u));
    }
    presenter.commitTransformEditForTesting(app);
    EXPECT_THAT(presenter.hasTransformEditForTesting(), testing::IsFalse());
    EXPECT_THAT(app.undoTimeline().entryCount(), testing::Eq(0u));
  }
  EXPECT_THAT(app.document().document().source(), testing::Eq(originalSource));
  presenter.refreshSnapshot(app);
  EXPECT_THAT(presenter.inspectorBoundsForTesting(),
              testing::Optional(testing::Eq(Box2d::FromXYWH(10, 20, 100, 50))));
}

TEST(SidebarTransformEditTest, StartingAnotherFieldCommitsThePreviousEditOnce) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kInspectorSvg));
  presenter.beginTransformEditForTesting(app, SidebarPresenter::TransformField::PositionX);
  ASSERT_THAT(presenter.applyTransformEditForTesting(app, 35.0), testing::IsTrue());
  ASSERT_THAT(app.flushFrame(), testing::IsTrue());
  presenter.refreshSnapshot(app);
  presenter.beginTransformEditForTesting(app, SidebarPresenter::TransformField::Height);
  EXPECT_THAT(app.undoTimeline().entryCount(), testing::Eq(1u));
  ASSERT_THAT(presenter.applyTransformEditForTesting(app, 75.0), testing::IsTrue());
  presenter.commitTransformEditForTesting(app);
  ASSERT_THAT(app.flushFrame(), testing::IsTrue());
  presenter.refreshSnapshot(app);
  EXPECT_THAT(app.undoTimeline().entryCount(), testing::Eq(2u));
  EXPECT_THAT(presenter.inspectorBoundsForTesting(),
              testing::Optional(testing::Eq(Box2d::FromXYWH(35, 20, 100, 75))));
  app.undo();
  ASSERT_THAT(app.flushFrame(), testing::IsTrue());
  presenter.refreshSnapshot(app);
  EXPECT_THAT(presenter.inspectorBoundsForTesting(),
              testing::Optional(testing::Eq(Box2d::FromXYWH(35, 20, 100, 50))));
  app.undo();
  ASSERT_THAT(app.flushFrame(), testing::IsTrue());
  presenter.refreshSnapshot(app);
  EXPECT_THAT(presenter.inspectorBoundsForTesting(),
              testing::Optional(testing::Eq(Box2d::FromXYWH(10, 20, 100, 50))));
}

TEST(SidebarTransformEditTest, ReloadDiscardsThePreviousDocumentsPendingEdit) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kInspectorSvg));
  presenter.beginTransformEditForTesting(app, SidebarPresenter::TransformField::PositionY);
  ASSERT_THAT(presenter.hasTransformEditForTesting(), testing::IsTrue());
  constexpr std::string_view replacement =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <rect id="target" x="2" y="3" width="7" height="11"/>
         </svg>)";
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, replacement));
  EXPECT_THAT(presenter.hasTransformEditForTesting(), testing::IsFalse());
  EXPECT_THAT(presenter.applyTransformEditForTesting(app, 99.0), testing::IsFalse());
  presenter.commitTransformEditForTesting(app);
  EXPECT_THAT(app.document().queue().size(), testing::Eq(0u));
  EXPECT_THAT(app.undoTimeline().entryCount(), testing::Eq(0u));
  EXPECT_THAT(presenter.inspectorBoundsForTesting(),
              testing::Optional(testing::Eq(Box2d::FromXYWH(2, 3, 7, 11))));
  EXPECT_THAT(app.document().document().source(), testing::Eq(replacement));
}

TEST(SidebarPresenterTest, DashSubmissionRejectsNegativeLengthsAndAdditionalDeclarations) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kInspectorSvg));
  const std::string originalSource(app.document().document().source());
  for (const std::string_view pattern : {"-1 2", "4; fill: blue", "4; stroke: black"}) {
    SCOPED_TRACE(pattern);
    EXPECT_THAT(presenter.submitDashPatternForTesting(app, pattern), testing::IsFalse());
    EXPECT_THAT(app.document().queue().size(), testing::Eq(0u));
    EXPECT_THAT(app.undoTimeline().entryCount(), testing::Eq(0u));
    EXPECT_THAT(app.document().document().source(), testing::Eq(originalSource));
  }
}

TEST(SidebarPresenterTest, ZeroDashLengthsAreRetainedAsAValidSvgStyle) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kInspectorSvg));
  const auto target = app.selectedElement();
  ASSERT_THAT(target, testing::Optional(testing::_));
  ASSERT_THAT(presenter.submitDashPatternForTesting(app, "0 0"), testing::IsTrue());
  ASSERT_THAT(app.flushFrame(), testing::IsTrue());
  EXPECT_THAT(target->getAttribute("style"),
              testing::Optional(testing::Eq(RcString("stroke-dasharray: 0 0"))));
  EXPECT_THAT(app.undoTimeline().entryCount(), testing::Eq(1u));
}

TEST(SidebarPresenterTest, SignedDashOffsetsKeepPercentageUnitsAndRejectNonFiniteValues) {
  constexpr std::string_view source =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <rect id="target" width="40" height="30" stroke="black"
                 stroke-dasharray="4 2" stroke-dashoffset="20%"/>
         </svg>)";
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, source));
  const auto target = app.selectedElement();
  ASSERT_THAT(target, testing::Optional(testing::_));
  ASSERT_THAT(presenter.setStrokeDashOffsetForTesting(app, -12.5), testing::IsTrue());
  ASSERT_THAT(app.flushFrame(), testing::IsTrue());
  EXPECT_THAT(target->getAttribute("style"),
              testing::Optional(testing::Eq(RcString("stroke-dashoffset: -12.5%"))));
  const std::string editedSource(app.document().document().source());
  for (double value :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    SCOPED_TRACE(value);
    EXPECT_THAT(presenter.setStrokeDashOffsetForTesting(app, value), testing::IsFalse());
    EXPECT_THAT(app.document().queue().size(), testing::Eq(0u));
    EXPECT_THAT(app.document().document().source(), testing::Eq(editedSource));
  }
}

TEST(SidebarPresenterTest, MarkerPickerRetainsItsFirstHundredIdsAndReplacesThemAfterReload) {
  std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80"><defs>)";
  std::vector<std::string> expected;
  for (int i = 0; i < 101; ++i) {
    const std::string id = "arrow-" + std::to_string(i);
    source += "<marker id=\"" + id + "\"><path d=\"M0 0L4 2L0 4\"/></marker>";
    if (i < 100) {
      expected.push_back(id);
    }
  }
  source += R"(</defs><rect id="target" width="10" height="10"/></svg>)";
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, source));
  EXPECT_THAT(presenter.markerIdsForTesting(), testing::UnorderedElementsAreArray(expected));
  const std::size_t scans = presenter.markerScanCountForTesting();
  presenter.refreshSnapshot(app);
  EXPECT_THAT(presenter.markerScanCountForTesting(), testing::Eq(scans));
  constexpr std::string_view replacement =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <defs><marker id="replacement"><path d="M0 0L4 2L0 4"/></marker></defs>
           <rect id="target" width="10" height="10"/>
         </svg>)";
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, replacement));
  EXPECT_THAT(presenter.markerIdsForTesting(), ElementsAre("replacement"));
  EXPECT_THAT(presenter.markerScanCountForTesting(), testing::Eq(scans + 1u));
}

TEST(SidebarPresenterTest, MarkerPickerHonorsTheDocumentedNodeScanBudget) {
  std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">)";
  for (int i = 0; i < 4096; ++i) {
    source += "<g/>";
  }
  source += R"(<marker id="beyond-budget"><path d="M0 0L4 2L0 4"/></marker>
               <rect id="target" width="10" height="10"/></svg>)";
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, source));
  EXPECT_THAT(presenter.markerIdsForTesting(), IsEmpty());
  EXPECT_THAT(presenter.inspectorTitleForTesting(), testing::Eq("Selected: <rect id=\"target\">"));
  const std::size_t scans = presenter.markerScanCountForTesting();
  presenter.refreshSnapshot(app);
  EXPECT_THAT(presenter.markerScanCountForTesting(), testing::Eq(scans));
}

TEST(SidebarPresenterTest, ColorWrappersKeepTheirCssValueAndProvenance) {
  const struct {
    std::string_view serialized;
    std::string_view value;
    InspectorStyleState state;
  } cases[] = {
      {"Color(currentColor)", "currentColor", InspectorStyleState::Unspecified},
      {"Color(rgba(1, 2, 3, 255)) (set)", "rgba(1, 2, 3, 255)", InspectorStyleState::Set},
      {"Color(rgba(0, 0, 0, 255)) (default)", "rgba(0, 0, 0, 255)", InspectorStyleState::Default},
  };
  for (const auto& item : cases) {
    SCOPED_TRACE(item.serialized);
    EXPECT_THAT(
        FormatInspectorStyleValue(item.serialized),
        testing::AllOf(
            testing::Field("value", &InspectorStyleDisplayValue::value, testing::Eq(item.value)),
            testing::Field("state", &InspectorStyleDisplayValue::state, testing::Eq(item.state))));
  }
}

TEST(SidebarPresenterTest, EmptyDocumentClearsCapturedMarkerAndTransformState) {
  constexpr std::string_view source =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80"><defs><marker id="arrow"><path d="M0 0L4 2L0 4"/></marker></defs><rect id="target" width="10" height="10"/></svg>)";
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, source));
  presenter.beginTransformEditForTesting(app, SidebarPresenter::TransformField::PositionX);
  ASSERT_THAT(presenter.hasTransformEditForTesting(), testing::IsTrue());
  ASSERT_THAT(presenter.markerIdsForTesting(), ElementsAre("arrow"));
  const EditorApp empty;
  presenter.refreshSnapshot(empty);
  EXPECT_THAT(presenter.hasTreeSnapshotForTesting(), testing::IsFalse());
  EXPECT_THAT(presenter.inspectorHasSelectionForTesting(), testing::IsFalse());
  EXPECT_THAT(presenter.hasTransformEditForTesting(), testing::IsFalse());
  EXPECT_THAT(presenter.markerIdsForTesting(), IsEmpty());
  EXPECT_THAT(presenter.inspectorTitleForTesting(), IsEmpty());
  EXPECT_THAT(presenter.inspectorXmlAttributesForTesting(), IsEmpty());
  EXPECT_THAT(presenter.inspectorComputedStyleForTesting(), IsEmpty());
  EXPECT_THAT(presenter.inspectorComputedStyleSwatchesForTesting(), IsEmpty());
  EXPECT_THAT(presenter.inspectorBoundsForTesting(), testing::Eq(std::nullopt));
  EXPECT_THAT(presenter.inspectorTransformForTesting(), testing::Eq(std::nullopt));
  EXPECT_THAT(app.undoTimeline().entryCount(), testing::Eq(0u));
}

struct InspectorFrameCapture {
  std::string text;
  Box2d lastItem;
};

class SidebarPresenterImGuiTest : public ::testing::Test {
protected:
  void SetUp() override {
    IMGUI_CHECKVERSION();
    ctx_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400, 300);
    io.ConfigMacOSXBehaviors = false;
    io.ConfigDragClickToInputText = true;
    io.Fonts->Build();
  }

  void TearDown() override {
    if (ctx_ != nullptr) {
      ImGui::DestroyContext(ctx_);
      ctx_ = nullptr;
    }
  }

  ImGuiContext* ctx_ = nullptr;

  static bool RenderInspectorFrame(SidebarPresenter& presenter, EditorApp* app,
                                   const char* windowName,
                                   const ImVec2 mouse = ImVec2(-1.0f, -1.0f),
                                   bool mouseDown = false, float windowHeight = 280.0f,
                                   const SidebarPresenter::IconTextureProvider& provider = {},
                                   InspectorFrameCapture* capture = nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400, std::max(300.0f, windowHeight + 20.0f));
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(0, mouseDown);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(360, windowHeight), ImGuiCond_Always);
    ImGui::Begin(windowName, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    if (capture != nullptr) {
      ImGui::LogToBuffer(0);
    }
    const bool queuedMutation = presenter.renderInspector(app, ViewportState{}, provider);
    if (capture != nullptr) {
      capture->text = ImGui::GetCurrentContext()->LogBuffer.c_str();
      const ImVec2 min = ImGui::GetItemRectMin();
      const ImVec2 max = ImGui::GetItemRectMax();
      capture->lastItem = Box2d(Vector2d(min.x, min.y), Vector2d(max.x, max.y));
      ImGui::LogFinish();
    }
    ImGui::End();
    ImGui::Render();
    return queuedMutation;
  }

  /// Render the real tree widgets and capture the final visible row's hit rectangle.
  static Box2d RenderLastTreeRowFrame(SidebarPresenter& presenter, EditorApp* app,
                                      TreeViewState& state, const char* windowName,
                                      ImVec2 mouse = ImVec2(-1.0f, -1.0f), bool mouseDown = false,
                                      ImGuiKey modifier = ImGuiKey_None,
                                      const SidebarPresenter::IconTextureProvider& provider = {}) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, modifier == ImGuiMod_Ctrl);
    io.AddKeyEvent(ImGuiMod_Super, modifier == ImGuiMod_Super);
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(0, mouseDown);
    state.selectionChangedInTree = false;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_Always);
    ImGui::Begin(windowName, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    presenter.renderTreeView(app, state, provider);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const Box2d row(Vector2d(min.x, min.y), Vector2d(max.x, max.y));
    ImGui::End();
    ImGui::Render();
    return row;
  }

  static bool ReplaceFocusedInput(SidebarPresenter& presenter, EditorApp* app,
                                  const char* windowName, std::string_view text, ImVec2 mouse,
                                  float height, InspectorFrameCapture* capture = nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    RenderInspectorFrame(presenter, app, windowName, mouse, false, height);
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    RenderInspectorFrame(presenter, app, windowName, mouse, false, height);
    io.AddInputCharactersUTF8(std::string(text).c_str());
    return RenderInspectorFrame(presenter, app, windowName, mouse, false, height, {}, capture);
  }

  static std::vector<ImDrawVert> TextureVertices(ImTextureID texture) {
    std::vector<ImDrawVert> vertices;
    const ImDrawData* data = ImGui::GetDrawData();
    if (data == nullptr) {
      return vertices;
    }
    for (const ImDrawList* list : data->CmdLists) {
      for (const ImDrawCmd& command : list->CmdBuffer) {
        if (command.GetTexID() != texture) {
          continue;
        }
        for (unsigned int index = command.IdxOffset; index < command.IdxOffset + command.ElemCount;
             ++index) {
          vertices.push_back(list->VtxBuffer[command.VtxOffset + list->IdxBuffer[index]]);
        }
      }
    }
    return vertices;
  }

  static std::vector<ImU32> DrawVertexColors() {
    std::vector<ImU32> colors;
    const ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData == nullptr) {
      return colors;
    }
    colors.reserve(static_cast<std::size_t>(drawData->TotalVtxCount));
    for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex) {
      const ImDrawList* drawList = drawData->CmdLists[listIndex];
      for (const ImDrawVert& vertex : drawList->VtxBuffer) {
        colors.push_back(vertex.col);
      }
    }
    return colors;
  }
};

constexpr std::string_view kTreeClickSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80"><rect id="peer" width="10" height="10"/><rect id="target" x="20" width="10" height="10"/></svg>)";

TEST_F(SidebarPresenterImGuiTest, TreeRowClickReplacesSelectionAndConsumesTheScrollRequest) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kTreeClickSvg));
  const auto target = app.selectedElement();
  const auto peer = app.document().document().querySelector("#peer");
  ASSERT_THAT(target, testing::Optional(testing::_));
  ASSERT_THAT(peer, testing::Optional(testing::_));
  app.setSelection(*peer);
  presenter.refreshSnapshot(app);
  TreeViewState state{.scrollTarget = *target, .pendingScroll = true};
  constexpr char windowName[] = "##tree_single_click_contract";
  const Box2d row = RenderLastTreeRowFrame(presenter, &app, state, windowName);
  const ImVec2 center = RectCenter(row);
  RenderLastTreeRowFrame(presenter, &app, state, windowName, center);
  RenderLastTreeRowFrame(presenter, &app, state, windowName, center, true);
  RenderLastTreeRowFrame(presenter, &app, state, windowName, center, false);
  EXPECT_THAT(app.selectedElements(), ElementsAre(*target));
  EXPECT_THAT(state.selectionChangedInTree, testing::IsTrue());
  EXPECT_THAT(state.pendingScroll, testing::IsFalse());
  EXPECT_THAT(app.undoTimeline().entryCount(), testing::Eq(0u));
  EXPECT_THAT(app.document().document().source(), testing::Eq(kTreeClickSvg));
}

TEST_F(SidebarPresenterImGuiTest, TreeControlAndSuperClicksToggleMembershipWithoutSourceChanges) {
  for (ImGuiKey modifier : {ImGuiMod_Ctrl, ImGuiMod_Super}) {
    SCOPED_TRACE(static_cast<int>(modifier));
    EditorApp app;
    SidebarPresenter presenter;
    ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kTreeClickSvg));
    const auto target = app.selectedElement();
    const auto peer = app.document().document().querySelector("#peer");
    ASSERT_THAT(target, testing::Optional(testing::_));
    ASSERT_THAT(peer, testing::Optional(testing::_));
    app.setSelection(*peer);
    presenter.refreshSnapshot(app);
    TreeViewState state{.scrollTarget = *target, .pendingScroll = true};
    const char* windowName =
        modifier == ImGuiMod_Ctrl ? "##tree_ctrl_contract" : "##tree_super_contract";
    const Box2d row = RenderLastTreeRowFrame(presenter, &app, state, windowName);
    const ImVec2 center = RectCenter(row);
    RenderLastTreeRowFrame(presenter, &app, state, windowName, center, false, modifier);
    RenderLastTreeRowFrame(presenter, &app, state, windowName, center, true, modifier);
    RenderLastTreeRowFrame(presenter, &app, state, windowName, center, false, modifier);
    EXPECT_THAT(app.selectedElements(), testing::UnorderedElementsAre(*peer, *target));
    EXPECT_THAT(state.selectionChangedInTree, testing::IsTrue());
    presenter.refreshSnapshot(app);
    RenderLastTreeRowFrame(presenter, &app, state, windowName, center, true, modifier);
    RenderLastTreeRowFrame(presenter, &app, state, windowName, center, false, modifier);
    EXPECT_THAT(app.selectedElements(), ElementsAre(*peer));
    EXPECT_THAT(state.selectionChangedInTree, testing::IsTrue());
    EXPECT_THAT(app.undoTimeline().entryCount(), testing::Eq(0u));
    EXPECT_THAT(app.document().document().source(), testing::Eq(kTreeClickSvg));
    RenderLastTreeRowFrame(presenter, &app, state, windowName, ImVec2(-1.0f, -1.0f));
  }
}

TEST_F(SidebarPresenterImGuiTest, ReadOnlyTreeDropsClicksAndKeepsTheScrollRequestPending) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kTreeClickSvg));
  const auto target = app.selectedElement();
  const auto peer = app.document().document().querySelector("#peer");
  ASSERT_THAT(target, testing::Optional(testing::_));
  ASSERT_THAT(peer, testing::Optional(testing::_));
  app.setSelection(*peer);
  presenter.refreshSnapshot(app);
  TreeViewState state{.scrollTarget = *target, .pendingScroll = true};
  constexpr char windowName[] = "##tree_readonly_click_contract";
  const Box2d row = RenderLastTreeRowFrame(presenter, nullptr, state, windowName);
  const ImVec2 center = RectCenter(row);
  RenderLastTreeRowFrame(presenter, nullptr, state, windowName, center);
  RenderLastTreeRowFrame(presenter, nullptr, state, windowName, center, true);
  RenderLastTreeRowFrame(presenter, nullptr, state, windowName, center, false);
  EXPECT_THAT(app.selectedElements(), ElementsAre(*peer));
  EXPECT_THAT(state.selectionChangedInTree, testing::IsFalse());
  EXPECT_THAT(state.pendingScroll, testing::IsTrue());
  EXPECT_THAT(app.document().queue().size(), testing::Eq(0u));
  EXPECT_THAT(app.document().document().source(), testing::Eq(kTreeClickSvg));
}

TEST_F(SidebarPresenterImGuiTest, ScrollRevealExpandsOnlyTheTargetAncestors) {
  constexpr std::string_view source =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80"><g id="outer"><g id="inner"><rect id="target" width="10" height="10"/></g></g><g id="unrelated"><rect width="10" height="10"/></g></svg>)";
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, source));
  const auto target = app.selectedElement();
  ASSERT_THAT(target, testing::Optional(testing::_));
  TreeViewState state{.scrollTarget = *target, .pendingScroll = true};
  RenderLastTreeRowFrame(presenter, nullptr, state, "##nested_tree_reveal_contract");
  const svg::SVGElement root = app.document().document().svgElement();
  EXPECT_THAT(presenter.isTreeNodeExpandedForTesting(
                  static_cast<std::uint32_t>(root.unsafeEntityHandle().entity())),
              testing::IsTrue());
  for (const auto selector : {"#outer", "#inner"}) {
    SCOPED_TRACE(selector);
    const auto element = app.document().document().querySelector(selector);
    ASSERT_THAT(element, testing::Optional(testing::_));
    EXPECT_THAT(presenter.isTreeNodeExpandedForTesting(
                    static_cast<std::uint32_t>(element->unsafeEntityHandle().entity())),
                testing::IsTrue());
  }
  const auto unrelated = app.document().document().querySelector("#unrelated");
  ASSERT_THAT(unrelated, testing::Optional(testing::_));
  EXPECT_THAT(presenter.isTreeNodeExpandedForTesting(
                  static_cast<std::uint32_t>(unrelated->unsafeEntityHandle().entity())),
              testing::IsFalse());
  EXPECT_THAT(state.selectionChangedInTree, testing::IsFalse());
  EXPECT_THAT(app.selectedElements(), ElementsAre(*target));
}

TEST_F(SidebarPresenterImGuiTest, StrokeWidthDecrementPreservesTheAuthoredEmUnit) {
  constexpr std::string_view source =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80"><rect id="target" width="40" height="30" stroke="black" stroke-width="3em"/></svg>)";
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, source));
  const auto target = app.selectedElement();
  ASSERT_THAT(target, testing::Optional(testing::_));
  constexpr char windowName[] = "##stroke_width_em_decrement_contract";
  RenderInspectorFrame(presenter, &app, windowName, ImVec2(-1.0f, -1.0f), false, 560.0f);
  const auto decrement = presenter.strokeDecrementRectForTesting();
  ASSERT_THAT(decrement, testing::Optional(testing::_));
  RenderInspectorFrame(presenter, &app, windowName, RectCenter(*decrement), true, 560.0f);
  ASSERT_THAT(
      RenderInspectorFrame(presenter, &app, windowName, RectCenter(*decrement), false, 560.0f),
      testing::IsTrue());
  ASSERT_THAT(app.flushFrame(), testing::IsTrue());
  EXPECT_THAT(target->getAttribute("style"),
              testing::Optional(testing::Eq(RcString("stroke-width: 2em"))));
  EXPECT_THAT(app.undoTimeline().entryCount(), testing::Eq(1u));
}

TEST_F(SidebarPresenterImGuiTest, MiterLimitDecrementRecordsOneUndoableStyleChange) {
  constexpr std::string_view source =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80"><rect id="target" width="40" height="30" stroke="black" stroke-miterlimit="5"/></svg>)";
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, source));
  const auto target = app.selectedElement();
  ASSERT_THAT(target, testing::Optional(testing::_));
  constexpr char windowName[] = "##miter_decrement_contract";
  RenderInspectorFrame(presenter, &app, windowName, ImVec2(-1.0f, -1.0f), false, 560.0f);
  const auto decrement = presenter.strokeMiterDecrementRectForTesting();
  ASSERT_THAT(decrement, testing::Optional(testing::_));
  EXPECT_THAT(decrement->bottomRight.y, testing::Lt(560.0));
  RenderInspectorFrame(presenter, &app, windowName, RectCenter(*decrement), true, 560.0f);
  ASSERT_THAT(
      RenderInspectorFrame(presenter, &app, windowName, RectCenter(*decrement), false, 560.0f),
      testing::IsTrue());
  ASSERT_THAT(app.flushFrame(), testing::IsTrue());
  EXPECT_THAT(target->getAttribute("style"),
              testing::Optional(testing::Eq(RcString("stroke-miterlimit: 4"))));
  EXPECT_THAT(app.undoTimeline().entryCount(), testing::Eq(1u));
  app.undo();
  ASSERT_THAT(app.flushFrame(), testing::IsTrue());
  const auto restored = app.document().document().querySelector("#target");
  ASSERT_THAT(restored, testing::Optional(testing::_));
  EXPECT_THAT(restored->getAttribute("style"), testing::Eq(std::nullopt));
  EXPECT_THAT(restored->getAttribute("stroke-miterlimit"),
              testing::Optional(testing::Eq(RcString("5"))));
}

TEST_F(SidebarPresenterImGuiTest, StrokeTypingDeactivatedWhileBusyCommitsWhenLiveAccessReturns) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kInspectorSvg));
  const auto target = app.selectedElement();
  ASSERT_THAT(target, testing::Optional(testing::_));
  constexpr char windowName[] = "##stroke_busy_commit_contract";
  RenderInspectorFrame(presenter, &app, windowName);
  const auto field = presenter.strokeWidthRectForTesting();
  ASSERT_THAT(field, testing::Optional(testing::_));
  RenderInspectorFrame(presenter, &app, windowName, RectCenter(*field), true);
  RenderInspectorFrame(presenter, &app, windowName, RectCenter(*field), false);
  ImGui::GetIO().AddInputCharacter('3');
  ASSERT_THAT(RenderInspectorFrame(presenter, &app, windowName), testing::IsTrue());
  ASSERT_THAT(app.flushFrame(), testing::IsTrue());
  EXPECT_THAT(target->getAttribute("style"),
              testing::Optional(testing::Eq(RcString("stroke-width: 3"))));
  EXPECT_THAT(app.undoTimeline().entryCount(), testing::Eq(0u));
  const ImVec2 outside(20.0f, 20.0f);
  EXPECT_THAT(RenderInspectorFrame(presenter, nullptr, windowName, outside, true),
              testing::IsFalse());
  EXPECT_THAT(RenderInspectorFrame(presenter, nullptr, windowName, outside, false),
              testing::IsFalse());
  EXPECT_THAT(app.undoTimeline().entryCount(), testing::Eq(0u));
  EXPECT_THAT(RenderInspectorFrame(presenter, &app, windowName), testing::IsFalse());
  EXPECT_THAT(app.flushFrame(), testing::IsFalse());
  EXPECT_THAT(app.undoTimeline().entryCount(), testing::Eq(1u));
  app.undo();
  ASSERT_THAT(app.flushFrame(), testing::IsTrue());
  const auto restored = app.document().document().querySelector("#target");
  ASSERT_THAT(restored, testing::Optional(testing::_));
  EXPECT_THAT(restored->getAttribute("style"), testing::Eq(std::nullopt));
}

TEST_F(SidebarPresenterImGuiTest, TreeViewAndInspectorRenderEmptySnapshotReadOnly) {
  SidebarPresenter presenter;
  TreeViewState treeState;
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(400, 300);
  io.AddMousePosEvent(-1.0f, -1.0f);
  io.AddMouseButtonEvent(0, false);

  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_Always);
  ImGui::Begin("##sidebar_empty_snapshot_test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
  presenter.renderTreeView(nullptr, treeState);
  const bool queuedMutation = presenter.renderInspector(nullptr, ViewportState{});
  ImGui::End();
  ImGui::Render();

  const ImDrawData* drawData = ImGui::GetDrawData();
  EXPECT_FALSE(queuedMutation);
  EXPECT_FALSE(treeState.selectionChangedInTree);
  ASSERT_NE(drawData, nullptr);
  EXPECT_GT(drawData->TotalVtxCount, 0);
}

TEST_F(SidebarPresenterImGuiTest, InspectorRendersMultiSelectionAndSingleSelectionSnapshots) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);

  const auto target = app.document().document().querySelector("#target");
  const auto peer = app.document().document().querySelector("#peer");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(peer.has_value());

  SidebarPresenter presenter;
  app.setSelection(std::vector<svg::SVGElement>{*target, *peer});
  presenter.refreshSnapshot(app);

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(400, 300);
  io.AddMousePosEvent(-1.0f, -1.0f);
  io.AddMouseButtonEvent(0, false);

  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_Always);
  ImGui::Begin("##sidebar_multi_selection_test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
  EXPECT_FALSE(presenter.renderInspector(&app, ViewportState{}));
  ImGui::End();
  ImGui::Render();
  const ImDrawData* multiDrawData = ImGui::GetDrawData();
  ASSERT_NE(multiDrawData, nullptr);
  EXPECT_GT(multiDrawData->TotalVtxCount, 0);

  app.setSelection(*target);
  presenter.refreshSnapshot(app);

  io.AddMousePosEvent(-1.0f, -1.0f);
  io.AddMouseButtonEvent(0, false);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_Always);
  ImGui::Begin("##sidebar_single_selection_test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
  EXPECT_FALSE(presenter.renderInspector(&app, ViewportState{}));
  ImGui::End();
  ImGui::Render();
  const ImDrawData* singleDrawData = ImGui::GetDrawData();
  ASSERT_NE(singleDrawData, nullptr);
  EXPECT_GT(singleDrawData->TotalVtxCount, 0);
}

TEST_F(SidebarPresenterImGuiTest, BusyFrameKeepsInspectorSnapshotAppearance) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##sidebar_busy_snapshot_appearance_test";

  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName));
  const std::vector<ImU32> idleColors = DrawVertexColors();
  ASSERT_FALSE(idleColors.empty());
  const std::string title = std::string(presenter.inspectorTitleForTesting());

  ASSERT_FALSE(RenderInspectorFrame(presenter, nullptr, kWindowName));
  const std::vector<ImU32> busyColors = DrawVertexColors();
  EXPECT_EQ(busyColors, idleColors)
      << "Busy frames should replay the captured inspector appearance";
  EXPECT_EQ(presenter.inspectorTitleForTesting(), title);
  EXPECT_THAT(presenter.inspectorXmlAttributesForTesting(),
              ElementsAre(Pair("x", "10"), Pair("y", "20"), Pair("width", "100"),
                          Pair("height", "50"), Pair("fill", "red"), Pair("id", "target")));
  EXPECT_FALSE(app.canUndo());
}

TEST_F(SidebarPresenterImGuiTest, StrokeWidthStepUpdatesEverySelectedElement) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);
  const auto target = app.document().document().querySelector("#target");
  const auto peer = app.document().document().querySelector("#peer");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(peer.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*target, *peer});

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##sidebar_stroke_step_test";
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName));
  const auto step = presenter.strokeIncrementRectForTesting();
  const auto downStep = presenter.strokeDecrementRectForTesting();
  const auto field = presenter.strokeWidthRectForTesting();
  ASSERT_TRUE(step.has_value());
  ASSERT_TRUE(downStep.has_value());
  ASSERT_TRUE(field.has_value());
  EXPECT_NEAR(field->bottomRight.x, step->topLeft.x, 0.001);
  EXPECT_NEAR(field->topLeft.y, step->topLeft.y, 0.001);
  EXPECT_NEAR(step->bottomRight.y, downStep->topLeft.y, 0.001);
  EXPECT_NEAR(field->bottomRight.y, downStep->bottomRight.y, 0.001);
  const ImVec2 center(static_cast<float>((step->topLeft.x + step->bottomRight.x) * 0.5),
                      static_cast<float>((step->topLeft.y + step->bottomRight.y) * 0.5));

  RenderInspectorFrame(presenter, &app, kWindowName, center, /*mouseDown=*/true);
  EXPECT_TRUE(RenderInspectorFrame(presenter, &app, kWindowName, center, /*mouseDown=*/false));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(target->getAttribute("style"), "stroke-width: 2");
  EXPECT_EQ(peer->getAttribute("style"), "stroke-width: 2");
  ASSERT_TRUE(app.canUndo());
  app.undo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(app.document().document().querySelector("#target")->getAttribute("style"),
            std::nullopt);
  app.redo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(app.document().document().querySelector("#target")->getAttribute("style"),
            "stroke-width: 2");
}

TEST_F(SidebarPresenterImGuiTest, WidthFieldFocusOpensEditableTrueScalePresetPopup) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##sidebar_width_presets_test";
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, ImVec2(-1.0f, -1.0f),
                                    /*mouseDown=*/false, /*windowHeight=*/600.0f));
  const auto field = presenter.strokeWidthRectForTesting();
  ASSERT_TRUE(field.has_value());
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*field), /*mouseDown=*/true,
                       /*windowHeight=*/600.0f);
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*field), /*mouseDown=*/false,
                       /*windowHeight=*/600.0f);
  EXPECT_TRUE(ImGui::GetIO().WantTextInput) << "single click should keep numeric typing active";
  const auto preset = presenter.strokeWidthPresetRectForTesting(6);
  ASSERT_TRUE(preset.has_value()) << "focused width input should expand the preset menu";
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*preset), /*mouseDown=*/true,
                       /*windowHeight=*/600.0f);
  EXPECT_TRUE(RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*preset),
                                   /*mouseDown=*/false, /*windowHeight=*/600.0f));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(target->getAttribute("style"), "stroke-width: 8");
}

TEST_F(SidebarPresenterImGuiTest, WidthPresetPopupNeverCoversItsFieldInShortWindows) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##sidebar_short_width_presets_test";
  RenderInspectorFrame(presenter, &app, kWindowName, ImVec2(-1.0f, -1.0f), /*mouseDown=*/false,
                       /*windowHeight=*/600.0f);
  const auto tallField = presenter.strokeWidthRectForTesting();
  ASSERT_TRUE(tallField.has_value());

  // Too little room below the field for the whole preset list, as in a short editor window.
  const float shortHeight = std::ceil(static_cast<float>(tallField->bottomRight.y)) + 12.0f;
  RenderInspectorFrame(presenter, &app, kWindowName, ImVec2(-1.0f, -1.0f), /*mouseDown=*/false,
                       shortHeight);
  const auto field = presenter.strokeWidthRectForTesting();
  const auto stepper = presenter.strokeDecrementRectForTesting();
  ASSERT_TRUE(field.has_value());
  ASSERT_TRUE(stepper.has_value());
  const Box2d control(field->topLeft, Vector2d(stepper->bottomRight.x, field->bottomRight.y));
  const float displayHeight = ImGui::GetIO().DisplaySize.y;

  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*field), /*mouseDown=*/true,
                       shortHeight);
  for (int frame = 0; frame < 3; ++frame) {
    RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*field), /*mouseDown=*/false,
                         shortHeight);
  }
  const std::optional<Box2d> popup = OpenPopupWindowRect();
  ASSERT_TRUE(popup.has_value()) << "focusing the width field should open its presets";
  EXPECT_THAT(*popup, DoesNotOverlap(control));
  EXPECT_THAT(popup->topLeft.y, testing::Ge(0.0));
  EXPECT_THAT(popup->bottomRight.y, testing::Le(static_cast<double>(displayHeight)));

  // A second click on the field places the caret instead of choosing a preset under the pointer.
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*field), /*mouseDown=*/true,
                       shortHeight);
  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*field),
                                    /*mouseDown=*/false, shortHeight));
  EXPECT_FALSE(app.flushFrame()) << "clicking the field should not apply a preset";
  EXPECT_THAT(target->getAttribute("style"), testing::Eq(std::nullopt));
  EXPECT_TRUE(ImGui::GetIO().WantTextInput) << "the width field should keep keyboard focus";
}

TEST_F(SidebarPresenterImGuiTest, WidthEnterCommitsTypedValueAndClosesPresets) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##sidebar_width_enter_test";
  constexpr float kHeight = 600.0f;
  RenderInspectorFrame(presenter, &app, kWindowName, ImVec2(-1.0f, -1.0f), /*mouseDown=*/false,
                       kHeight);
  const auto field = presenter.strokeWidthRectForTesting();
  ASSERT_TRUE(field.has_value());
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*field), /*mouseDown=*/true,
                       kHeight);
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*field), /*mouseDown=*/false,
                       kHeight);
  ReplaceFocusedInput(presenter, &app, kWindowName, "6", RectCenter(*field), kHeight);
  ASSERT_TRUE(OpenPopupWindowRect().has_value()) << "presets stay open while typing";

  ImGuiIO& io = ImGui::GetIO();
  io.AddKeyEvent(ImGuiKey_Enter, true);
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*field), /*mouseDown=*/false,
                       kHeight);
  io.AddKeyEvent(ImGuiKey_Enter, false);
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*field), /*mouseDown=*/false,
                       kHeight);
  ASSERT_TRUE(app.flushFrame());

  EXPECT_THAT(target->getAttribute("style"), testing::Optional(testing::Eq("stroke-width: 6")));
  EXPECT_THAT(OpenPopupWindowRect(), testing::Eq(std::nullopt)) << "Enter should close presets";
  EXPECT_THAT(presenter.strokeWidthPresetRectForTesting(0), testing::Eq(std::nullopt));
  EXPECT_FALSE(io.WantTextInput) << "Enter should end width typing";
}

TEST_F(SidebarPresenterImGuiTest, WidthEscapeClosesPresets) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##sidebar_width_escape_test";
  constexpr float kHeight = 600.0f;
  RenderInspectorFrame(presenter, &app, kWindowName, ImVec2(-1.0f, -1.0f), /*mouseDown=*/false,
                       kHeight);
  const auto field = presenter.strokeWidthRectForTesting();
  ASSERT_TRUE(field.has_value());
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*field), /*mouseDown=*/true,
                       kHeight);
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*field), /*mouseDown=*/false,
                       kHeight);
  ReplaceFocusedInput(presenter, &app, kWindowName, "9", RectCenter(*field), kHeight);
  ASSERT_TRUE(OpenPopupWindowRect().has_value()) << "presets stay open while typing";

  ImGuiIO& io = ImGui::GetIO();
  io.AddKeyEvent(ImGuiKey_Escape, true);
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*field), /*mouseDown=*/false,
                       kHeight);
  io.AddKeyEvent(ImGuiKey_Escape, false);
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*field), /*mouseDown=*/false,
                       kHeight);
  (void)app.flushFrame();

  EXPECT_THAT(OpenPopupWindowRect(), testing::Eq(std::nullopt)) << "Escape should close presets";
  EXPECT_THAT(presenter.strokeWidthPresetRectForTesting(0), testing::Eq(std::nullopt));
  EXPECT_FALSE(io.WantTextInput) << "Escape should end width typing";
}

TEST_F(SidebarPresenterImGuiTest, MiterLimitStepsMatchFieldHeightAndUpdateStyle) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##sidebar_miter_step_test";
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName));
  const auto field = presenter.strokeMiterLimitRectForTesting();
  const auto up = presenter.strokeMiterIncrementRectForTesting();
  const auto down = presenter.strokeMiterDecrementRectForTesting();
  ASSERT_TRUE(field.has_value());
  ASSERT_TRUE(up.has_value());
  ASSERT_TRUE(down.has_value());
  EXPECT_NEAR(field->bottomRight.x, up->topLeft.x, 0.001);
  EXPECT_NEAR(field->topLeft.y, up->topLeft.y, 0.001);
  EXPECT_NEAR(up->bottomRight.y, down->topLeft.y, 0.001);
  EXPECT_NEAR(field->bottomRight.y, down->bottomRight.y, 0.001);

  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*up), /*mouseDown=*/true);
  EXPECT_TRUE(RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*up),
                                   /*mouseDown=*/false));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(target->getComputedStyle().strokeMiterlimit.get().value(), 5.0);
}

TEST_F(SidebarPresenterImGuiTest, LockedSelectionKeepsStrokeControlsReadOnly) {
  constexpr std::string_view kLockedSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <g data-donner-locked="true"><rect id="locked" width="40" height="40"
              stroke="black" stroke-width="5"/></g>
           <rect id="unlocked" x="60" width="40" height="40" stroke="black"/>
         </svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kLockedSvg));
  const auto locked = app.document().document().querySelector("#locked");
  const auto unlocked = app.document().document().querySelector("#unlocked");
  ASSERT_TRUE(locked.has_value());
  ASSERT_TRUE(unlocked.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*locked, *unlocked});

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##locked_stroke_controls";
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName));
  const auto step = presenter.strokeIncrementRectForTesting();
  const auto cap = presenter.strokeCapRectForTesting(1);
  ASSERT_TRUE(step.has_value());
  ASSERT_TRUE(cap.has_value());
  for (const Box2d& control : {*step, *cap}) {
    EXPECT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(control), true));
    EXPECT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(control), false));
  }
  EXPECT_EQ(app.document().queue().size(), 0u);
  EXPECT_FALSE(app.canUndo());
  EXPECT_EQ(locked->getComputedStyle().strokeWidth.get().value(), Lengthd(5));
  EXPECT_EQ(unlocked->getAttribute("style"), std::nullopt);
}

TEST_F(SidebarPresenterImGuiTest, WidthStepperPreservesEmUnitAndUndoRedo) {
  constexpr std::string_view kRelativeWidthSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <rect id="target" width="100" height="50" font-size="16"
                 stroke="black" stroke-width="2em"/>
         </svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRelativeWidthSvg));
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);
  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##relative_width_step";
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName));
  const auto step = presenter.strokeIncrementRectForTesting();
  ASSERT_TRUE(step.has_value());
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*step), true);
  EXPECT_TRUE(RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*step), false));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(target->getAttribute("style"), "stroke-width: 3em");
  ASSERT_TRUE(app.canUndo());
  app.undo();
  ASSERT_TRUE(app.flushFrame());
  const auto undone = app.document().document().querySelector("#target");
  ASSERT_TRUE(undone.has_value());
  EXPECT_EQ(undone->getAttribute("style"), std::nullopt);
  EXPECT_EQ(undone->getComputedStyle().strokeWidth.get().value(), Lengthd(2, LengthUnit::Em));
  app.redo();
  ASSERT_TRUE(app.flushFrame());
  const auto redone = app.document().document().querySelector("#target");
  ASSERT_TRUE(redone.has_value());
  EXPECT_EQ(redone->getComputedStyle().strokeWidth.get().value(), Lengthd(3, LengthUnit::Em));
}

TEST_F(SidebarPresenterImGuiTest, MultiFrameWidthTypingRecordsOneUndoEntry) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);
  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##stroke_width_typing_undo";
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName));
  const auto field = presenter.strokeWidthRectForTesting();
  ASSERT_TRUE(field.has_value());
  const ImVec2 center = RectCenter(*field);
  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, center, true));
  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, center, false));
  ImGui::GetIO().AddInputCharacter('3');
  EXPECT_TRUE(RenderInspectorFrame(presenter, &app, kWindowName));
  EXPECT_TRUE(ImGui::GetIO().WantTextInput);
  ASSERT_TRUE(app.flushFrame());
  ImGui::GetIO().AddInputCharacter('4');
  RenderInspectorFrame(presenter, &app, kWindowName);
  EXPECT_TRUE(ImGui::GetIO().WantTextInput);
  EXPECT_EQ(app.undoTimeline().entryCount(), 0u);
  // Leaving the field commits both updates as one undoable edit.
  const ImVec2 outside(20.0f, 20.0f);
  RenderInspectorFrame(presenter, &app, kWindowName, outside, true);
  RenderInspectorFrame(presenter, &app, kWindowName, outside, false);
  app.flushFrame();
  const std::string afterTyping(app.document().document().source());
  EXPECT_EQ(target->getAttribute("style"), "stroke-width: 34");
  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
  app.undo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(app.document().document().querySelector("#target")->getAttribute("style"),
            std::nullopt);
  app.redo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(std::string(app.document().document().source()), afterTyping);
}

TEST_F(SidebarPresenterImGuiTest, DashOffsetEditPreservesEmUnit) {
  constexpr std::string_view kRelativeOffsetSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <path id="target" d="M10 40 L110 40" stroke="black"
                 stroke-dasharray="4 2" stroke-dashoffset="2em"/>
         </svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRelativeOffsetSvg));
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);
  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##relative_dash_offset";
  constexpr float kTallInspector = 560.0f;
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, ImVec2(-1.0f, -1.0f), false,
                                    kTallInspector));
  const auto offset = presenter.strokeDashOffsetRectForTesting();
  ASSERT_TRUE(offset.has_value());
  ASSERT_TRUE(presenter.setStrokeDashOffsetForTesting(app, 3.0));
  ASSERT_TRUE(app.flushFrame());
  const Lengthd computedOffset = target->getComputedStyle().strokeDashoffset.get().value();
  EXPECT_EQ(computedOffset.unit, LengthUnit::Em);
  EXPECT_EQ(computedOffset.value, 3.0);
}

TEST_F(SidebarPresenterImGuiTest, SegmentedStrokeChoicesAlignAndApplyCss) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);
  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##sidebar_stroke_segments_test";
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName));

  const auto butt = presenter.strokeCapRectForTesting(0);
  const auto round = presenter.strokeCapRectForTesting(1);
  const auto square = presenter.strokeCapRectForTesting(2);
  const auto join = presenter.strokeJoinRectForTesting(2);
  ASSERT_TRUE(butt.has_value());
  ASSERT_TRUE(round.has_value());
  ASSERT_TRUE(square.has_value());
  ASSERT_TRUE(join.has_value());
  EXPECT_NEAR(butt->topLeft.y, round->topLeft.y, 0.01);
  EXPECT_NEAR(round->topLeft.y, square->topLeft.y, 0.01);
  EXPECT_LT(butt->bottomRight.x, round->topLeft.x);
  EXPECT_LT(round->bottomRight.x, square->topLeft.x);
  EXPECT_GT(join->topLeft.y, butt->topLeft.y);
  EXPECT_TRUE(presenter.strokeMiterLimitRectForTesting().has_value());

  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*round), /*mouseDown=*/true);
  EXPECT_TRUE(RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*round),
                                   /*mouseDown=*/false));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_TRUE(target->getAttribute("style").has_value());
  EXPECT_THAT(std::string(std::string_view(*target->getAttribute("style"))),
              ::testing::HasSubstr("stroke-linecap: round"));

  presenter.refreshSnapshot(app);
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName));
  const auto roundJoin = presenter.strokeJoinRectForTesting(2);
  ASSERT_TRUE(roundJoin.has_value());
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*roundJoin), /*mouseDown=*/true);
  EXPECT_TRUE(RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*roundJoin),
                                   /*mouseDown=*/false));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_TRUE(target->getAttribute("style").has_value());
  EXPECT_THAT(std::string(std::string_view(*target->getAttribute("style"))),
              ::testing::HasSubstr("stroke-linejoin: round"));
  presenter.refreshSnapshot(app);
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName));
  EXPECT_FALSE(presenter.strokeMiterLimitRectForTesting().has_value());
}

TEST_F(SidebarPresenterImGuiTest, DashedLineToggleAppliesAndRestoresPattern) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);
  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##sidebar_dash_toggle_test";
  constexpr float kTallInspector = 520.0f;
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, ImVec2(-1.0f, -1.0f), false,
                                    kTallInspector));
  const auto toggle = presenter.strokeDashToggleRectForTesting();
  ASSERT_TRUE(toggle.has_value());
  ASSERT_LT(toggle->bottomRight.y, kTallInspector) << *toggle;
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*toggle), /*mouseDown=*/true,
                       kTallInspector);
  EXPECT_TRUE(RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*toggle),
                                   /*mouseDown=*/false, kTallInspector));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(target->getAttribute("style"), "stroke-dasharray: 4 2");
  ASSERT_TRUE(app.hasSelection());

  presenter.refreshSnapshot(app);
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, ImVec2(-1.0f, -1.0f), false,
                                    kTallInspector));
  const auto enabledToggle = presenter.strokeDashToggleRectForTesting();
  ASSERT_TRUE(enabledToggle.has_value());
  EXPECT_LT(enabledToggle->bottomRight.y, kTallInspector) << *enabledToggle;
  EXPECT_EQ(enabledToggle->topLeft.y, toggle->topLeft.y);
  const bool queuedOnDown =
      RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*enabledToggle),
                           /*mouseDown=*/true, kTallInspector);
  EXPECT_FALSE(queuedOnDown);
  EXPECT_TRUE(RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*enabledToggle),
                                   /*mouseDown=*/false, kTallInspector));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(target->getAttribute("style"), "stroke-dasharray: none");
}

TEST_F(SidebarPresenterImGuiTest, DashPreviewAndPresetReplaceCustomPattern) {
  constexpr std::string_view kCustomDashSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <path id="target" d="M10 40 L110 40" stroke="black"
                 stroke-dasharray="44 14"/>
         </svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kCustomDashSvg));
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);
  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##sidebar_dash_preset_test";
  constexpr float kTallInspector = 560.0f;
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, ImVec2(-1.0f, -1.0f), false,
                                    kTallInspector));
  const auto preview = presenter.strokeDashPreviewRectForTesting();
  const auto dotPreset = presenter.strokeDashPresetRectForTesting(1);
  ASSERT_TRUE(preview.has_value());
  ASSERT_TRUE(dotPreset.has_value());
  EXPECT_TRUE(presenter.strokeCustomDashSelectedForTesting());
  EXPECT_GT(preview->width(), dotPreset->width());
  EXPECT_GT(dotPreset->topLeft.y, preview->topLeft.y);
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*dotPreset), /*mouseDown=*/true,
                       kTallInspector);
  EXPECT_TRUE(RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*dotPreset),
                                   /*mouseDown=*/false, kTallInspector));
  ASSERT_TRUE(app.flushFrame());
  const auto style = target->getAttribute("style");
  ASSERT_TRUE(style.has_value());
  EXPECT_THAT(std::string(std::string_view(*style)), ::testing::HasSubstr("stroke-dasharray: 1 4"));
  presenter.refreshSnapshot(app);
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, ImVec2(-1.0f, -1.0f), false,
                                    kTallInspector));
  EXPECT_FALSE(presenter.strokeCustomDashSelectedForTesting());
}

TEST(SidebarPresenterTest, InvalidDashPatternDoesNotQueueStyleMutation) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kInspectorSvg));
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  EXPECT_FALSE(presenter.submitDashPatternForTesting(app, "4 bogus"));
  EXPECT_EQ(app.document().queue().size(), 0u);
  EXPECT_EQ(target->getAttribute("style"), std::nullopt);

  ASSERT_TRUE(presenter.submitDashPatternForTesting(app, "4 2"));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(target->getAttribute("style"), "stroke-dasharray: 4 2");
  ASSERT_TRUE(presenter.submitDashPatternForTesting(app, "none"));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(target->getAttribute("style"), "stroke-dasharray: none");
}

TEST_F(SidebarPresenterImGuiTest, LongExistingDashPatternIsVisibleAndReadOnly) {
  std::string pattern;
  for (int i = 0; i < 70; ++i) {
    if (!pattern.empty()) {
      pattern += ' ';
    }
    pattern += "1 2";
  }
  const std::string svg =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"80\">"
      "<rect id=\"target\" width=\"100\" height=\"50\" stroke=\"black\" "
      "stroke-dasharray=\"" +
      pattern + "\"/></svg>";
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, svg));
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  ASSERT_GT(pattern.size(), 127u);
  EXPECT_FALSE(presenter.dashPatternEditableForTesting());
  EXPECT_FALSE(presenter.submitDashPatternForTesting(app, pattern));
  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, "##long_dash_pattern"));
  EXPECT_EQ(app.document().queue().size(), 0u);
  EXPECT_EQ(target->getAttribute("stroke-dasharray"), pattern);
  EXPECT_EQ(target->getAttribute("style"), std::nullopt);

  const auto computedBefore = target->getComputedStyle().strokeDasharray.get();
  ASSERT_TRUE(computedBefore.has_value());
  std::ostringstream originalPattern;
  originalPattern << *computedBefore;
  constexpr char kWindowName[] = "##long_dash_toggle";
  constexpr float kTallInspector = 560.0f;
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, ImVec2(-1.0f, -1.0f), false,
                                    kTallInspector));
  const auto toggle = presenter.strokeDashToggleRectForTesting();
  ASSERT_TRUE(toggle.has_value());
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*toggle), true, kTallInspector);
  EXPECT_TRUE(RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*toggle), false,
                                   kTallInspector));
  ASSERT_TRUE(app.flushFrame());
  presenter.refreshSnapshot(app);
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, ImVec2(-1.0f, -1.0f), false,
                                    kTallInspector));
  const auto offToggle = presenter.strokeDashToggleRectForTesting();
  ASSERT_TRUE(offToggle.has_value());
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*offToggle), true, kTallInspector);
  EXPECT_TRUE(RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*offToggle), false,
                                   kTallInspector));
  ASSERT_TRUE(app.flushFrame());
  const auto restored = target->getComputedStyle().strokeDasharray.get();
  ASSERT_TRUE(restored.has_value());
  std::ostringstream restoredPattern;
  restoredPattern << *restored;
  EXPECT_EQ(restoredPattern.str(), originalPattern.str());
}

TEST(SidebarPresenterTest, MarkerIdsAreCachedAndRefreshAfterSourceMutation) {
  constexpr std::string_view kMarkerSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <defs><marker id="arrow"><path d="M0,0 L4,2 L0,4"/></marker></defs>
           <path id="target" d="M10,10 L100,10" stroke="black"/>
         </svg>)";
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kMarkerSvg));
  EXPECT_THAT(presenter.markerIdsForTesting(), ElementsAre("arrow"));
  const std::size_t scans = presenter.markerScanCountForTesting();
  presenter.refreshSnapshot(app);
  EXPECT_EQ(presenter.markerScanCountForTesting(), scans);

  const auto marker = app.document().document().querySelector("#arrow");
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(marker.has_value());
  ASSERT_TRUE(target.has_value());
  app.setSelection(*marker);
  ASSERT_TRUE(app.setAttributeOnSelection("id", "new-arrow"));
  ASSERT_TRUE(app.flushFrame());
  app.setSelection(*target);
  presenter.refreshSnapshot(app);
  EXPECT_THAT(presenter.markerIdsForTesting(), ElementsAre("new-arrow"));
  EXPECT_EQ(presenter.markerScanCountForTesting(), scans + 1u);
}

TEST(SidebarPresenterTest, ModifiedTaggedMarkerRemainsAvailableByDocumentId) {
  constexpr std::string_view source =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <defs><marker id="donner-marker-open-arrow" data-donner-prefab-marker="open-arrow">
             <path d="M1 1 L7 4 L1 7" fill="red"/>
           </marker></defs>
           <path id="target" d="M10 40 L110 40" stroke="black"/>
         </svg>)";
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, source));
  EXPECT_THAT(presenter.markerIdsForTesting(), ElementsAre("donner-marker-open-arrow"));
}

TEST_F(SidebarPresenterImGuiTest, MarkerPresetPickerInsertsArrowIntoDocument) {
  constexpr std::string_view kLineSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <path id="target" d="M10 40 L110 40" fill="none" stroke="black"/>
         </svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kLineSvg));
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);
  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##marker_prefab_picker_test";
  constexpr float kWindowHeight = 900.0f;
  RenderInspectorFrame(presenter, &app, kWindowName, ImVec2(-1.0f, -1.0f), false, kWindowHeight);
  const auto disclosure = presenter.markerDisclosureRectForTesting();
  ASSERT_TRUE(disclosure.has_value());
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*disclosure), true, kWindowHeight);
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*disclosure), false, kWindowHeight);
  const auto endPicker = presenter.markerPickerRectForTesting(1);
  ASSERT_TRUE(endPicker.has_value());
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*endPicker), true, kWindowHeight);
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*endPicker), false, kWindowHeight);
  RenderInspectorFrame(presenter, &app, kWindowName, ImVec2(-1.0f, -1.0f), false, kWindowHeight);
  const auto arrow = presenter.markerPrefabRectForTesting(0);
  ASSERT_TRUE(arrow.has_value());
  RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*arrow), true, kWindowHeight);
  EXPECT_TRUE(
      RenderInspectorFrame(presenter, &app, kWindowName, RectCenter(*arrow), false, kWindowHeight));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_TRUE(app.document().document().querySelector("#donner-marker-filled-arrow").has_value());
  presenter.refreshSnapshot(app);
  EXPECT_THAT(presenter.markerIdsForTesting(), IsEmpty());
  EXPECT_THAT(std::string(app.document().document().source()),
              HasSubstr("marker-end: url(#donner-marker-filled-arrow)"));
}

TEST_F(SidebarPresenterImGuiTest, BusyStrokeWidthStepDoesNotQueueMutation) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);
  const auto target = app.document().document().querySelector("#target");
  const auto peer = app.document().document().querySelector("#peer");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(peer.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*target, *peer});

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##sidebar_busy_stroke_step_test";
  ASSERT_FALSE(RenderInspectorFrame(presenter, nullptr, kWindowName));
  const auto step = presenter.strokeIncrementRectForTesting();
  ASSERT_TRUE(step.has_value());
  const ImVec2 center(static_cast<float>((step->topLeft.x + step->bottomRight.x) * 0.5),
                      static_cast<float>((step->topLeft.y + step->bottomRight.y) * 0.5));

  EXPECT_FALSE(RenderInspectorFrame(presenter, nullptr, kWindowName, center, /*mouseDown=*/true));
  EXPECT_FALSE(RenderInspectorFrame(presenter, nullptr, kWindowName, center, /*mouseDown=*/false));
  EXPECT_EQ(app.document().queue().size(), 0u);
  EXPECT_EQ(target->getAttribute("style"), std::nullopt);
}

TEST_F(SidebarPresenterImGuiTest, TreeViewOpensAncestorsForPendingScrollTarget) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);

  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  TreeViewState treeState;
  treeState.scrollTarget = *target;
  treeState.pendingScroll = true;

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(400, 300);
  io.AddMousePosEvent(-1.0f, -1.0f);
  io.AddMouseButtonEvent(0, false);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_Always);
  ImGui::Begin("##sidebar_tree_scroll_target_test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
  presenter.renderTreeView(nullptr, treeState);
  ImGui::End();
  ImGui::Render();

  EXPECT_TRUE(treeState.pendingScroll);
  EXPECT_FALSE(treeState.selectionChangedInTree);
  const ImDrawData* drawData = ImGui::GetDrawData();
  ASSERT_NE(drawData, nullptr);
  EXPECT_GT(drawData->TotalVtxCount, 0);
}

TEST_F(SidebarPresenterImGuiTest, TreeDisclosureStateRoundTripsForInspectorTree) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  const svg::SVGElement root = app.document().document().svgElement();
  const std::uint32_t rootId = static_cast<std::uint32_t>(root.unsafeEntityHandle().entity());

  // The disclosure state that the shared chevron drives round-trips: collapsed
  // by default (matching the former imgui default-closed node), expands on
  // toggle, and collapses again.
  EXPECT_FALSE(presenter.isTreeNodeExpandedForTesting(rootId));
  presenter.toggleTreeNodeExpandedForTesting(rootId);
  EXPECT_TRUE(presenter.isTreeNodeExpandedForTesting(rootId));

  TreeViewState treeState;
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(400, 300);
  io.AddMousePosEvent(-1.0f, -1.0f);
  io.AddMouseButtonEvent(0, false);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_Always);
  ImGui::Begin("##sidebar_tree_disclosure_roundtrip_test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
  presenter.renderTreeView(nullptr, treeState);
  ImGui::End();
  ImGui::Render();
  const ImDrawData* drawData = ImGui::GetDrawData();
  ASSERT_NE(drawData, nullptr);
  EXPECT_GT(drawData->TotalVtxCount, 0);

  presenter.toggleTreeNodeExpandedForTesting(rootId);
  EXPECT_FALSE(presenter.isTreeNodeExpandedForTesting(rootId));
}

TEST_F(SidebarPresenterImGuiTest, InspectorRendersSingleSelectionWithoutElementDetails) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(R"(<svg xmlns="http://www.w3.org/2000/svg">
    <defs/>
  </svg>)"));

  const auto defs = app.document().document().querySelector("defs");
  ASSERT_TRUE(defs.has_value());
  app.setSelection(*defs);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  ASSERT_TRUE(presenter.inspectorHasSelectionForTesting());
  EXPECT_EQ(presenter.inspectorTitleForTesting(), "Selected: <defs>");
  EXPECT_TRUE(presenter.inspectorXmlAttributesForTesting().empty());

  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, "##sidebar_defs_inspector_test"));
  const ImDrawData* drawData = ImGui::GetDrawData();
  ASSERT_NE(drawData, nullptr);
  EXPECT_GT(drawData->TotalVtxCount, 0);
}

TEST_F(SidebarPresenterImGuiTest, InspectorRendersLiveNoSelectionState) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  ASSERT_FALSE(presenter.inspectorHasSelectionForTesting());

  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, "##sidebar_no_selection_live_test"));
  const ImDrawData* drawData = ImGui::GetDrawData();
  ASSERT_NE(drawData, nullptr);
  EXPECT_GT(drawData->TotalVtxCount, 0);
}

TEST_F(SidebarPresenterImGuiTest, PathOperationButtonsRenderSvgBitmapIcons) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="a" x="0" y="0" width="20" height="20"/>
    <rect id="b" x="10" y="10" width="20" height="20"/>
  </svg>)"));

  const std::optional<svg::SVGElement> a = app.document().document().querySelector("#a");
  const std::optional<svg::SVGElement> b = app.document().document().querySelector("#b");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*a, *b});

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  constexpr ImTextureID kIconTexture = static_cast<ImTextureID>(0x9876);
  int providerCalls = 0;
  int pathCalls = 0;
  int strokeCalls = 0;
  int nonEmptyBitmaps = 0;
  int retinaBitmaps = 0;
  const SidebarPresenter::IconTextureProvider iconTextureProvider =
      [&](std::uint64_t stableId, const svg::RendererBitmap& bitmap) {
        ++providerCalls;
        pathCalls +=
            std::ranges::any_of(kInspectorPathOperations, [&](PathOperationKind operation) {
              return stableId == PathOperationIconTextureKey(operation);
            });
        strokeCalls += std::ranges::any_of(kStrokePreviewIcons, [&](StrokePreviewIcon icon) {
          return stableId == StrokePreviewIconTextureKey(icon);
        });
        if (!bitmap.empty() && bitmap.dimensions.x > 0 && bitmap.dimensions.y > 0) {
          ++nonEmptyBitmaps;
        }
        if (!bitmap.empty() && bitmap.dimensions.x >= 36 && bitmap.dimensions.y >= 36) {
          ++retinaBitmaps;
        }
        return SidebarPresenter::IconTexture{
            .texture = kIconTexture,
            .uvBottomRight = Vector2d(1.0, 1.0),
        };
      };

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(400, 300);
  io.AddMousePosEvent(-1.0f, -1.0f);
  io.AddMouseButtonEvent(0, false);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_Always);
  ImGui::Begin("##sidebar_path_icons_test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
  presenter.renderInspector(&app, ViewportState{}, iconTextureProvider);
  const ImDrawList* drawList = ImGui::GetWindowDrawList();
  int imageQuads = 0;
  for (int cmdIndex = 0; cmdIndex < drawList->CmdBuffer.Size; ++cmdIndex) {
    const ImDrawCmd& cmd = drawList->CmdBuffer[cmdIndex];
    if (cmd.GetTexID() == kIconTexture) {
      imageQuads += static_cast<int>(cmd.ElemCount / 6u);
    }
  }
  ImGui::End();
  ImGui::Render();

  EXPECT_EQ(pathCalls, 4)
      << "the path operation UI should request Union, Intersect, Subtract Front, and Exclude";
  EXPECT_EQ(strokeCalls, 6) << "the stroke controls should request all six SVG previews";
  EXPECT_EQ(providerCalls, pathCalls + strokeCalls);
  EXPECT_EQ(nonEmptyBitmaps, providerCalls)
      << "inspector icons must receive Donner-rendered SVG bitmaps";
  EXPECT_EQ(retinaBitmaps, providerCalls)
      << "inspector icons must be rasterized above their display size";
  EXPECT_EQ(imageQuads, providerCalls) << "every inspector icon should render as an image quad";
}

TEST_F(SidebarPresenterImGuiTest, InspectorRendersEditableTransformFields) {
  constexpr std::string_view kRotatedSvg =
      R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <rect id="target" x="10" y="20" width="40" height="30" transform="rotate(30)"/>
         </svg>)SVG";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRotatedSvg));
  app.setCleanSourceText(kRotatedSvg);

  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  ASSERT_TRUE(presenter.inspectorHasSelectionForTesting());

  // Rendering the editable fields must not queue a mutation when nothing is
  // activated, and must draw geometry for the added widgets.
  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, "##sidebar_transform_fields_test"));
  const ImDrawData* drawData = ImGui::GetDrawData();
  ASSERT_NE(drawData, nullptr);
  EXPECT_GT(drawData->TotalVtxCount, 0);
  EXPECT_FALSE(app.canUndo()) << "rendering alone must not record undo entries";
}

TEST_F(SidebarPresenterImGuiTest, TransformAppearsBeforeStroke) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, "##sidebar_section_order_test"));

  const auto transform =
      presenter.transformFieldRectForTesting(SidebarPresenter::TransformField::PositionX);
  const auto stroke = presenter.strokeIncrementRectForTesting();
  ASSERT_TRUE(transform.has_value());
  ASSERT_TRUE(stroke.has_value());
  EXPECT_LT(transform->bottomRight.y, stroke->topLeft.y);
}

TEST_F(SidebarPresenterImGuiTest, TransformFieldsUseAlignedValueColumns) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, "##sidebar_transform_alignment_test"));

  const auto x =
      presenter.transformFieldRectForTesting(SidebarPresenter::TransformField::PositionX);
  const auto y =
      presenter.transformFieldRectForTesting(SidebarPresenter::TransformField::PositionY);
  const auto width =
      presenter.transformFieldRectForTesting(SidebarPresenter::TransformField::Width);
  const auto height =
      presenter.transformFieldRectForTesting(SidebarPresenter::TransformField::Height);
  const auto rotation =
      presenter.transformFieldRectForTesting(SidebarPresenter::TransformField::Rotation);
  ASSERT_TRUE(x.has_value());
  ASSERT_TRUE(y.has_value());
  ASSERT_TRUE(width.has_value());
  ASSERT_TRUE(height.has_value());
  ASSERT_TRUE(rotation.has_value());

  EXPECT_DOUBLE_EQ(x->topLeft.x, width->topLeft.x);
  EXPECT_DOUBLE_EQ(x->bottomRight.x, width->bottomRight.x);
  EXPECT_DOUBLE_EQ(x->topLeft.x, rotation->topLeft.x);
  EXPECT_DOUBLE_EQ(x->bottomRight.x, rotation->bottomRight.x);
  EXPECT_DOUBLE_EQ(y->topLeft.x, height->topLeft.x);
  EXPECT_DOUBLE_EQ(y->bottomRight.x, height->bottomRight.x);
}

TEST_F(SidebarPresenterImGuiTest, TransformFieldSimpleClickEntersTextInput) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##sidebar_transform_click_to_edit_test";
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName));
  const auto x =
      presenter.transformFieldRectForTesting(SidebarPresenter::TransformField::PositionX);
  ASSERT_TRUE(x.has_value());
  const ImVec2 center(static_cast<float>((x->topLeft.x + x->bottomRight.x) * 0.5),
                      static_cast<float>((x->topLeft.y + x->bottomRight.y) * 0.5));

  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, center, /*mouseDown=*/true));
  EXPECT_TRUE(presenter.hasTransformEditForTesting());
  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, center, /*mouseDown=*/false));

  ASSERT_NE(ImGui::GetCurrentContext(), nullptr);
  EXPECT_NE(ImGui::GetCurrentContext()->TempInputId, 0u)
      << "A click-release without dragging should enter numeric text input";
  EXPECT_TRUE(presenter.hasTransformEditForTesting());
  EXPECT_FALSE(app.canUndo()) << "Entering text mode alone must not mutate the document";
}

TEST_F(SidebarPresenterImGuiTest, NumericInputRetainsDigitsAndCommitWhileDocumentIsBusy) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);
  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindow[] = "##numeric_input_busy_document";
  (void)RenderInspectorFrame(presenter, &app, kWindow);
  const auto bounds =
      presenter.transformFieldRectForTesting(SidebarPresenter::TransformField::PositionX);
  ASSERT_TRUE(bounds.has_value());
  const auto center = RectCenter(*bounds);
  (void)RenderInspectorFrame(presenter, &app, kWindow, center, false);
  (void)RenderInspectorFrame(presenter, &app, kWindow, center, true);
  (void)RenderInspectorFrame(presenter, &app, kWindow, center, false);
  ASSERT_NE(ImGui::GetCurrentContext()->TempInputId, 0u);

  ImGuiIO& io = ImGui::GetIO();
  io.AddInputCharactersUTF8("6");
  (void)RenderInspectorFrame(presenter, &app, kWindow, center, false);
  (void)app.flushFrame();
  io.AddInputCharactersUTF8("4");
  EXPECT_FALSE(RenderInspectorFrame(presenter, nullptr, kWindow, center, false));
  io.AddKeyEvent(ImGuiKey_Enter, true);
  EXPECT_FALSE(RenderInspectorFrame(presenter, nullptr, kWindow, center, false));
  io.AddKeyEvent(ImGuiKey_Enter, false);
  EXPECT_FALSE(RenderInspectorFrame(presenter, nullptr, kWindow, center, false));
  (void)RenderInspectorFrame(presenter, &app, kWindow, center, false);
  (void)app.flushFrame();
  const auto geometry = target->cast<svg::SVGGeometryElement>().worldBounds();
  ASSERT_TRUE(geometry.has_value());
  EXPECT_DOUBLE_EQ(geometry->topLeft.x, 64.0);
  EXPECT_FALSE(presenter.hasTransformEditForTesting());
  EXPECT_TRUE(app.canUndo());
}
TEST_F(SidebarPresenterImGuiTest, PendingNumericInputCannotCrossDocumentReplacement) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);
  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindow[] = "##numeric_input_replaced_document";
  (void)RenderInspectorFrame(presenter, &app, kWindow);
  const auto bounds =
      presenter.transformFieldRectForTesting(SidebarPresenter::TransformField::PositionX);
  ASSERT_TRUE(bounds.has_value());
  const auto center = RectCenter(*bounds);
  (void)RenderInspectorFrame(presenter, &app, kWindow, center, false);
  (void)RenderInspectorFrame(presenter, &app, kWindow, center, true);
  (void)RenderInspectorFrame(presenter, &app, kWindow, center, false);
  ASSERT_NE(ImGui::GetCurrentContext()->TempInputId, 0u);

  ImGuiIO& io = ImGui::GetIO();
  io.AddInputCharactersUTF8("6");
  (void)RenderInspectorFrame(presenter, &app, kWindow, center, false);
  (void)app.flushFrame();
  io.AddInputCharactersUTF8("4");
  EXPECT_FALSE(RenderInspectorFrame(presenter, nullptr, kWindow, center, false));
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  const auto replacement = app.document().document().querySelector("#target");
  ASSERT_TRUE(replacement.has_value());
  app.setSelection(*replacement);
  presenter.refreshSnapshot(app);
  (void)RenderInspectorFrame(presenter, &app, kWindow, center, false);
  (void)app.flushFrame();
  EXPECT_FALSE(presenter.hasTransformEditForTesting());
  EXPECT_FALSE(app.canUndo()) << "A replaced document must not receive the old edit's history";
  const auto oldBounds = target->cast<svg::SVGGeometryElement>().worldBounds();
  ASSERT_TRUE(oldBounds.has_value());
  EXPECT_DOUBLE_EQ(oldBounds->topLeft.x, 6.0)
      << "The pending digit belongs to the replaced document and must be discarded";
}

TEST_F(SidebarPresenterImGuiTest, InspectorRendersSkewedTransformDisabledFields) {
  constexpr std::string_view kSkewedSvg =
      R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <rect id="target" x="10" y="20" width="40" height="30" transform="skewX(20)"/>
         </svg>)SVG";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSkewedSvg));
  app.setCleanSourceText(kSkewedSvg);

  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  ASSERT_TRUE(presenter.inspectorHasSelectionForTesting());

  // A skewed matrix is not decomposable; the fields render disabled and the
  // frame must not crash or queue mutations.
  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, "##sidebar_transform_skew_test"));
  const ImDrawData* drawData = ImGui::GetDrawData();
  ASSERT_NE(drawData, nullptr);
  EXPECT_GT(drawData->TotalVtxCount, 0);
  EXPECT_FALSE(app.canUndo());
}

TEST_F(SidebarPresenterImGuiTest, InspectorRendersTransformFieldsWithoutLiveApp) {
  constexpr std::string_view kRotatedSvg =
      R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <rect id="target" x="10" y="20" width="40" height="30" transform="rotate(30)"/>
         </svg>)SVG";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRotatedSvg));
  app.setCleanSourceText(kRotatedSvg);

  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  // Null live app (async renderer busy): the snapshot renders read-only.
  EXPECT_FALSE(RenderInspectorFrame(presenter, nullptr, "##sidebar_transform_busy_test"));
  const ImDrawData* drawData = ImGui::GetDrawData();
  ASSERT_NE(drawData, nullptr);
  EXPECT_GT(drawData->TotalVtxCount, 0);
}

/// Render one inspector frame with the raw-matrix disclosure forced open by
/// pre-seeding the tree node's persistent open state in the window storage.
bool RenderInspectorFrameWithMatrixOpen(SidebarPresenter& presenter, EditorApp* app,
                                        const char* windowName) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(400, 300);
  io.AddMousePosEvent(-1.0f, -1.0f);
  io.AddMouseButtonEvent(0, false);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_Always);
  ImGui::Begin(windowName, nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
  ImGui::GetStateStorage()->SetInt(ImGui::GetID("Matrix##transform_matrix"), 1);
  const bool queuedMutation = presenter.renderInspector(app, ViewportState{});
  ImGui::End();
  ImGui::Render();
  return queuedMutation;
}

TEST_F(SidebarPresenterImGuiTest, MatrixDisclosureRendersRawComponentCells) {
  constexpr std::string_view kRotatedSvg =
      R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <rect id="target" x="10" y="20" width="40" height="30" transform="rotate(30)"/>
         </svg>)SVG";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRotatedSvg));
  app.setCleanSourceText(kRotatedSvg);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  ASSERT_TRUE(presenter.inspectorHasSelectionForTesting());
  constexpr char kWindowName[] = "##sidebar_matrix_disclosure_test";

  // Baseline frame with the disclosure collapsed.
  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName));
  for (int index = 0; index < 6; ++index) {
    EXPECT_FALSE(presenter.matrixFieldRectForTesting(index).has_value());
  }

  // Opening the disclosure builds all six raw matrix drag cells, still without
  // a queued mutation or undo entry.
  EXPECT_FALSE(RenderInspectorFrameWithMatrixOpen(presenter, &app, kWindowName));
  for (int index = 0; index < 6; ++index) {
    EXPECT_TRUE(presenter.matrixFieldRectForTesting(index).has_value())
        << "Matrix component " << index << " should have an ImGui item.";
  }
  EXPECT_FALSE(app.canUndo()) << "Rendering the matrix cells must not record undo entries.";

  // An in-progress matrix edit displays the edit-buffer value for its cell.
  presenter.beginTransformEditForTesting(app, SidebarPresenter::TransformField::Matrix,
                                         /*matrixIndex=*/2);
  EXPECT_FALSE(RenderInspectorFrameWithMatrixOpen(presenter, &app, kWindowName));
  presenter.commitTransformEditForTesting(app);
  EXPECT_FALSE(presenter.hasTransformEditForTesting());

  // Busy frame (no live app): the cells render disabled and stay inert.
  EXPECT_FALSE(RenderInspectorFrameWithMatrixOpen(presenter, nullptr, kWindowName));
  for (int index = 0; index < 6; ++index) {
    EXPECT_TRUE(presenter.matrixFieldRectForTesting(index).has_value());
  }
}

TEST_F(SidebarPresenterImGuiTest, PathOperationButtonsRenderDisabledWithoutLiveApp) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  constexpr ImTextureID kIconTexture = static_cast<ImTextureID>(0x5432);
  int pathCalls = 0;
  int strokeCalls = 0;
  const SidebarPresenter::IconTextureProvider iconTextureProvider =
      [&](std::uint64_t stableId, const svg::RendererBitmap&) {
        pathCalls +=
            std::ranges::any_of(kInspectorPathOperations, [&](PathOperationKind operation) {
              return stableId == PathOperationIconTextureKey(operation);
            });
        strokeCalls += std::ranges::any_of(kStrokePreviewIcons, [&](StrokePreviewIcon icon) {
          return stableId == StrokePreviewIconTextureKey(icon);
        });
        return SidebarPresenter::IconTexture{
            .texture = kIconTexture,
            .uvBottomRight = Vector2d(1.0, 1.0),
        };
      };

  // A busy frame (null live app) reports every operation unavailable, but the
  // buttons must still render (disabled) so the panel layout stays stable.
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(400, 300);
  io.AddMousePosEvent(-1.0f, -1.0f);
  io.AddMouseButtonEvent(0, false);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_Always);
  ImGui::Begin("##sidebar_path_ops_disabled_test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
  const bool queuedMutation =
      presenter.renderInspector(nullptr, ViewportState{}, iconTextureProvider);
  ImGui::End();
  ImGui::Render();

  EXPECT_FALSE(queuedMutation);
  EXPECT_EQ(pathCalls, 4) << "disabled path buttons still request all four icon textures";
  EXPECT_EQ(strokeCalls, 6) << "read-only stroke buttons still request their SVG previews";
  EXPECT_FALSE(app.canUndo());
}

TEST_F(SidebarPresenterImGuiTest, TransformEditCommitsWhenSelectionChanges) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);
  const auto target = app.document().document().querySelector("#target");
  const auto peer = app.document().document().querySelector("#peer");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(peer.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  presenter.beginTransformEditForTesting(app, SidebarPresenter::TransformField::PositionX);
  ASSERT_TRUE(presenter.applyTransformEditForTesting(app, 25.0));
  ASSERT_TRUE(presenter.hasTransformEditForTesting());

  // The live selection moved to a different element while the edit was still
  // composing: the next inspector frame must land the edit as an undo entry
  // instead of composing against a stale baseline.
  app.setSelection(*peer);
  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, "##sidebar_commit_on_selection_change"));
  EXPECT_FALSE(presenter.hasTransformEditForTesting())
      << "A selection change must commit the in-progress transform edit.";
  EXPECT_TRUE(app.canUndo()) << "The committed edit must be undoable.";
}

TEST_F(SidebarPresenterImGuiTest, TransformEditPendingCommitFinalizesOnNextLiveFrame) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  presenter.beginTransformEditForTesting(app, SidebarPresenter::TransformField::PositionX);
  ASSERT_TRUE(presenter.applyTransformEditForTesting(app, 30.0));

  // A busy frame (no live app) cannot commit; the edit is parked for commit.
  EXPECT_FALSE(RenderInspectorFrame(presenter, nullptr, "##sidebar_pending_commit_test"));
  EXPECT_TRUE(presenter.hasTransformEditForTesting())
      << "A busy frame must park the edit instead of dropping it.";

  // The next live frame lands the parked commit.
  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, "##sidebar_pending_commit_test"));
  EXPECT_FALSE(presenter.hasTransformEditForTesting())
      << "The parked edit must commit on the next frame with live app access.";
  EXPECT_TRUE(app.canUndo()) << "The committed edit must be undoable.";
}

TEST_F(SidebarPresenterImGuiTest, TransformFieldDragAppliesAndCommitsEdit) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindowName[] = "##sidebar_transform_drag_commit_test";
  ASSERT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName));
  const auto x =
      presenter.transformFieldRectForTesting(SidebarPresenter::TransformField::PositionX);
  ASSERT_TRUE(x.has_value());
  const ImVec2 center(static_cast<float>((x->topLeft.x + x->bottomRight.x) * 0.5),
                      static_cast<float>((x->topLeft.y + x->bottomRight.y) * 0.5));

  // Hover first so window-focus nav bookkeeping settles, then press: a click
  // delivered on the same frame as the nav-init result would be recorded as a
  // keyboard-sourced activation and ignore mouse drag deltas.
  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, center, /*mouseDown=*/false));
  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, center, /*mouseDown=*/true));
  EXPECT_TRUE(presenter.hasTransformEditForTesting());

  bool applied = false;
  ImVec2 dragged = center;
  for (int step = 1; step <= 4; ++step) {
    dragged.x = center.x + 10.0f * static_cast<float>(step);
    applied =
        RenderInspectorFrame(presenter, &app, kWindowName, dragged, /*mouseDown=*/true) || applied;
  }
  EXPECT_TRUE(applied) << "Dragging the X field must queue a live transform mutation.";

  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, dragged, /*mouseDown=*/false));
  EXPECT_FALSE(presenter.hasTransformEditForTesting())
      << "Releasing the drag must commit the transform edit.";
  EXPECT_TRUE(app.canUndo()) << "The committed drag edit must be undoable.";
}

// Each Path Operations button has to draw its own operation's artwork. Both
// halves of that contract are pinned here: distinct SVG sources so no two
// buttons share a glyph, and distinct texture keys so no two buttons share an
// uploaded presentation texture. The icons are Donner-rendered SVG - the SVG
// bytes are what the editor rasterizes - so a mapping regression is visible
// right here rather than only on screen.
TEST(SidebarPathOperationIcons, EveryInspectorOperationMapsToItsOwnIconAndTextureKey) {
  std::vector<std::pair<PathOperationKind, const unsigned char*>> iconSources;
  std::vector<std::uint64_t> textureKeys;
  for (const PathOperationKind operation : kInspectorPathOperations) {
    const std::span<const unsigned char> svg = PathOperationIconSvg(operation);
    ASSERT_THAT(svg, Not(IsEmpty())) << "operation " << static_cast<int>(operation);
    iconSources.emplace_back(operation, svg.data());
    textureKeys.push_back(PathOperationIconTextureKey(operation));
  }

  for (std::size_t i = 0; i < iconSources.size(); ++i) {
    for (std::size_t j = i + 1u; j < iconSources.size(); ++j) {
      EXPECT_NE(iconSources[i].second, iconSources[j].second)
          << "operations " << static_cast<int>(iconSources[i].first) << " and "
          << static_cast<int>(iconSources[j].first) << " share one icon asset";
      EXPECT_NE(textureKeys[i], textureKeys[j])
          << "operations " << static_cast<int>(iconSources[i].first) << " and "
          << static_cast<int>(iconSources[j].first) << " share one icon texture key";
    }
  }
}

// The prewarm batch is what the editor's boot path actually rasterizes, so it
// has to carry every button's artwork; a button missing from it pays a GPU
// readback on the first selection instead.
TEST(SidebarPathOperationIcons, PrewarmRequestsCoverEveryInspectorButton) {
  const std::span<const EmbeddedSvgIconRequest> requests = SidebarIconPrewarmRequests();
  for (const PathOperationKind operation : kInspectorPathOperations) {
    const std::span<const unsigned char> svg = PathOperationIconSvg(operation);
    const bool present =
        std::any_of(requests.begin(), requests.end(), [&](const EmbeddedSvgIconRequest& request) {
          return request.svgBytes.data() == svg.data() && request.tintableMask;
        });
    EXPECT_TRUE(present) << "operation " << static_cast<int>(operation)
                         << "'s icon is not in the startup prewarm batch";
  }
}

TEST(SidebarStrokePreviewIcons, SvgStrokesRenderDistinctCapsAndJoins) {
  constexpr std::array<std::string_view, 6> kAttributes = {
      "stroke-linecap=\"butt\"",   "stroke-linecap=\"round\"",  "stroke-linecap=\"square\"",
      "stroke-linejoin=\"miter\"", "stroke-linejoin=\"round\"", "stroke-linejoin=\"bevel\"",
  };
  std::array<std::vector<std::uint8_t>, kStrokePreviewIcons.size()> pixels;
  std::array<std::uint64_t, kStrokePreviewIcons.size()> textureKeys;
  for (std::size_t index = 0; index < kStrokePreviewIcons.size(); ++index) {
    const StrokePreviewIcon icon = kStrokePreviewIcons[index];
    const std::span<const unsigned char> source = StrokePreviewIconSvg(icon);
    ASSERT_FALSE(source.empty());
    const std::string_view svg(reinterpret_cast<const char*>(source.data()), source.size());
    EXPECT_NE(svg.find(kAttributes[index]), std::string_view::npos);
    const std::optional<svg::RendererBitmap> bitmap = RenderEmbeddedSvgArtwork(source, 96);
    ASSERT_TRUE(bitmap.has_value());
    ASSERT_FALSE(bitmap->empty());
    pixels[index] = bitmap->pixels;
    textureKeys[index] = StrokePreviewIconTextureKey(icon);
  }
  for (std::size_t first = 0; first < kStrokePreviewIcons.size(); ++first) {
    for (std::size_t second = first + 1u; second < kStrokePreviewIcons.size(); ++second) {
      EXPECT_NE(textureKeys[first], textureKeys[second]);
      if ((first < 3u) == (second < 3u)) {
        EXPECT_NE(pixels[first], pixels[second]) << "SVG preview variants must render differently";
      }
    }
  }
}

TEST(SidebarStrokePreviewIcons, WidthPresetSpriteUsesOnePixelPerUserUnitAtActualSize) {
  const std::optional<svg::RendererBitmap> bitmap =
      RenderEmbeddedSvgArtwork(StrokeWidthPresetSvg(), 256);
  ASSERT_TRUE(bitmap.has_value());
  ASSERT_EQ(bitmap->dimensions, Vector2i(256, 256));
  for (std::size_t index = 0; index < kStrokeWidthPresetValues.size(); ++index) {
    float alphaCoverage = 0.0f;
    const int rowStart = static_cast<int>(index) * 32;
    for (int y = rowStart; y < rowStart + 32; ++y) {
      const std::size_t offset = static_cast<std::size_t>(y) * bitmap->rowBytes + 16u * 4u;
      alphaCoverage += static_cast<float>(bitmap->pixels[offset + 3u]) / 255.0f;
    }
    EXPECT_LE(kStrokeWidthPresetValues[index], 12.0f);
    EXPECT_NEAR(alphaCoverage, kStrokeWidthPresetValues[index], 1.5f)
        << "preset " << index << " should show true-scale width inside its 32px row";
    EXPECT_LT(alphaCoverage, 16.0f) << "quick-width stroke must not fill its icon";
  }
}

TEST(SidebarStrokePreviewIcons, PrewarmRequestsIncludeEveryStrokeSvg) {
  const std::span<const EmbeddedSvgIconRequest> requests = SidebarIconPrewarmRequests();
  for (const StrokePreviewIcon icon : kStrokePreviewIcons) {
    const std::span<const unsigned char> source = StrokePreviewIconSvg(icon);
    const bool present =
        std::any_of(requests.begin(), requests.end(), [&](const EmbeddedSvgIconRequest& request) {
          return request.svgBytes.data() == source.data() && request.outputSizePx == 96 &&
                 !request.tintableMask;
        });
    EXPECT_TRUE(present) << "stroke SVG preview is missing from startup prewarm";
  }
}

TEST_F(SidebarPresenterImGuiTest, MiterTypingRejectsSmallValuesAndRecoversOneUndoableEdit) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kInspectorSvg));
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const std::string before(app.document().document().source());
  constexpr char kWindow[] = "##miter_validation_recovery";
  constexpr float kHeight = 900.0f;
  RenderInspectorFrame(presenter, &app, kWindow, ImVec2(-1, -1), false, kHeight);
  const auto field = presenter.strokeMiterLimitRectForTesting();
  ASSERT_TRUE(field.has_value());
  const auto center = RectCenter(*field);
  RenderInspectorFrame(presenter, &app, kWindow, center, false, kHeight);
  RenderInspectorFrame(presenter, &app, kWindow, center, true, kHeight);
  RenderInspectorFrame(presenter, &app, kWindow, center, false, kHeight);
  ASSERT_TRUE(ImGui::GetIO().WantTextInput);

  InspectorFrameCapture capture;
  EXPECT_FALSE(ReplaceFocusedInput(presenter, &app, kWindow, "0", center, kHeight));
  RenderInspectorFrame(presenter, &app, kWindow, ImVec2(-1, -1), false, kHeight, {}, &capture);
  ASSERT_THAT(capture.text, HasSubstr("STROKE"));
  EXPECT_THAT(capture.text, HasSubstr("Limit must be at least 1"));
  EXPECT_EQ(app.document().queue().size(), 0u);
  EXPECT_EQ(app.document().document().source(), before);
  EXPECT_FALSE(app.canUndo());
  EXPECT_TRUE(ReplaceFocusedInput(presenter, &app, kWindow, "3", center, kHeight));
  RenderInspectorFrame(presenter, &app, kWindow, ImVec2(-1, -1), false, kHeight, {}, &capture);
  ASSERT_THAT(capture.text, HasSubstr("STROKE"));
  EXPECT_THAT(capture.text, Not(HasSubstr("Limit must be at least 1")));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(target->getComputedStyle().strokeMiterlimit.get(), 3.0f);
  EXPECT_EQ(app.undoTimeline().entryCount(), 0u);
  RenderInspectorFrame(presenter, &app, kWindow, ImVec2(20, 20), true, kHeight);
  RenderInspectorFrame(presenter, &app, kWindow, ImVec2(20, 20), false, kHeight);
  app.flushFrame();
  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
  const std::string after(app.document().document().source());
  app.undo();
  app.flushFrame();
  EXPECT_EQ(app.document().document().source(), before);
  app.redo();
  app.flushFrame();
  EXPECT_EQ(app.document().document().source(), after);
}

TEST_F(SidebarPresenterImGuiTest, CustomDashTypingRejectsInvalidPatternThenClearsTheError) {
  constexpr std::string_view kDashed =
      R"(<svg xmlns="http://www.w3.org/2000/svg"><path id="target" d="M0 0L30 0" stroke="black" stroke-dasharray="4 2"/></svg>)";
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kDashed));
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  constexpr char kWindow[] = "##custom_dash_validation_recovery";
  constexpr float kHeight = 900.0f;
  RenderInspectorFrame(presenter, &app, kWindow, ImVec2(-1, -1), false, kHeight);
  const auto lastPreset = presenter.strokeDashPresetRectForTesting(2);
  ASSERT_TRUE(lastPreset.has_value());
  const ImVec2 custom(static_cast<float>(lastPreset->bottomRight.x) + 20.0f,
                      RectCenter(*lastPreset).y);
  RenderInspectorFrame(presenter, &app, kWindow, custom, true, kHeight);
  InspectorFrameCapture capture;
  RenderInspectorFrame(presenter, &app, kWindow, custom, false, kHeight, {}, &capture);
  ASSERT_THAT(capture.text, HasSubstr("Pattern"));
  const auto preview = presenter.strokeDashPreviewRectForTesting();
  ASSERT_TRUE(preview.has_value());
  const ImVec2 input(RectCenter(*preview).x, static_cast<float>(lastPreset->bottomRight.y) +
                                                 ImGui::GetStyle().ItemSpacing.y +
                                                 ImGui::GetFrameHeight() * 0.5f);
  RenderInspectorFrame(presenter, &app, kWindow, input, true, kHeight);
  RenderInspectorFrame(presenter, &app, kWindow, input, false, kHeight);
  ASSERT_TRUE(ImGui::GetIO().WantTextInput);
  const std::string before(app.document().document().source());
  EXPECT_FALSE(ReplaceFocusedInput(presenter, &app, kWindow, "4 -2", input, kHeight));
  ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, kWindow, input, false, kHeight, {}, &capture));
  RenderInspectorFrame(presenter, &app, kWindow, ImVec2(-1, -1), false, kHeight, {}, &capture);
  ASSERT_THAT(capture.text, HasSubstr("Pattern"));
  EXPECT_THAT(capture.text, HasSubstr("Use nonnegative dash and gap lengths"));
  EXPECT_EQ(app.document().queue().size(), 0u);
  EXPECT_EQ(app.document().document().source(), before);
  EXPECT_FALSE(app.canUndo());
  ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
  RenderInspectorFrame(presenter, &app, kWindow, input, false, kHeight);
  RenderInspectorFrame(presenter, &app, kWindow, input, true, kHeight);
  RenderInspectorFrame(presenter, &app, kWindow, input, false, kHeight);
  EXPECT_FALSE(ReplaceFocusedInput(presenter, &app, kWindow, "9 3", input, kHeight));
  ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
  EXPECT_TRUE(RenderInspectorFrame(presenter, &app, kWindow, input, false, kHeight, {}, &capture));
  RenderInspectorFrame(presenter, &app, kWindow, ImVec2(-1, -1), false, kHeight, {}, &capture);
  ASSERT_THAT(capture.text, HasSubstr("Pattern"));
  EXPECT_THAT(capture.text, Not(HasSubstr("Use nonnegative dash and gap lengths")));
  ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
  RenderInspectorFrame(presenter, &app, kWindow, input, false, kHeight);
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(target->getAttribute("style"), "stroke-dasharray: 9 3");
  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
  app.undo();
  app.flushFrame();
  EXPECT_EQ(app.document().document().source(), before);
}

TEST_F(SidebarPresenterImGuiTest, CustomEditorPreservesLongAuthoredDashPatternsReadOnly) {
  std::string pattern;
  for (int index = 0; index < 70; ++index) {
    pattern += index ? " 1 2" : "1 2";
  }
  const std::string source =
      "<svg xmlns=\"http://www.w3.org/2000/svg\"><path id=\"target\" d=\"M0 0L30 0\" "
      "stroke=\"black\" stroke-dasharray=\"" +
      pattern + "\"/></svg>";
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, source));
  constexpr char kWindow[] = "##long_custom_dash_readonly";
  constexpr float kHeight = 900.0f;
  RenderInspectorFrame(presenter, &app, kWindow, ImVec2(-1, -1), false, kHeight);
  const auto lastPreset = presenter.strokeDashPresetRectForTesting(2);
  ASSERT_TRUE(lastPreset.has_value());
  const ImVec2 custom(static_cast<float>(lastPreset->bottomRight.x) + 20.0f,
                      RectCenter(*lastPreset).y);
  RenderInspectorFrame(presenter, &app, kWindow, custom, true, kHeight);
  InspectorFrameCapture capture;
  EXPECT_FALSE(
      RenderInspectorFrame(presenter, &app, kWindow, custom, false, kHeight, {}, &capture));
  EXPECT_THAT(capture.text, HasSubstr("Long pattern - edit in SVG source"));
  EXPECT_FALSE(presenter.dashPatternEditableForTesting());
  EXPECT_EQ(app.document().queue().size(), 0u);
  EXPECT_EQ(app.document().document().source(), source);
  EXPECT_FALSE(app.canUndo());
  EXPECT_EQ(app.document().document().querySelector("#target")->getAttribute("stroke-dasharray"),
            pattern);
}

TEST_F(SidebarPresenterImGuiTest, ExcludeButtonMutatesOnlyWithLiveAccessAndKeepsGeometricMeaning) {
  constexpr std::string_view kOverlap =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40"><rect id="a" width="20" height="20"/><rect id="b" x="10" y="10" width="20" height="20"/></svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kOverlap));
  const auto a = app.document().document().querySelector("#a");
  const auto b = app.document().document().querySelector("#b");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*a, *b});
  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  constexpr char kWindow[] = "##exclude_action_live_gate";
  constexpr float kHeight = 900.0f;
  InspectorFrameCapture capture;
  RenderInspectorFrame(presenter, &app, kWindow, ImVec2(-1, -1), false, kHeight, {}, &capture);
  const ImVec2 button = RectCenter(capture.lastItem);
  const std::string before(app.document().document().source());
  EXPECT_FALSE(RenderInspectorFrame(presenter, nullptr, kWindow, button, true, kHeight));
  EXPECT_FALSE(RenderInspectorFrame(presenter, nullptr, kWindow, button, false, kHeight));
  EXPECT_EQ(app.document().document().source(), before);
  EXPECT_EQ(app.selectedElements().size(), 2u);
  EXPECT_FALSE(app.canUndo());
  RenderInspectorFrame(presenter, &app, kWindow, button, false, kHeight);
  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, kWindow, button, true, kHeight));
  EXPECT_TRUE(RenderInspectorFrame(presenter, &app, kWindow, button, false, kHeight));
  app.flushFrame();
  ASSERT_EQ(app.selectedElements().size(), 1u);
  const auto geometry =
      app.selectedElements().front().cast<svg::SVGGeometryElement>().computedSpline();
  ASSERT_TRUE(geometry.has_value());
  EXPECT_TRUE(geometry->isInside(Vector2d(5, 5)));
  EXPECT_TRUE(geometry->isInside(Vector2d(25, 25)));
  EXPECT_FALSE(geometry->isInside(Vector2d(15, 15)));
  EXPECT_FALSE(geometry->isInside(Vector2d(35, 35)));
  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
  app.undo();
  app.flushFrame();
  EXPECT_EQ(app.document().document().source(), before);
}

TEST_F(SidebarPresenterImGuiTest, TreeChevronTextureUsesValidUvsAndDisclosureDoesNotSelect) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  const auto root = app.document().document().svgElement();
  const auto rootId = static_cast<std::uint32_t>(root.unsafeEntityHandle().entity());
  constexpr ImTextureID kTexture = static_cast<ImTextureID>(0x4567);
  std::vector<std::uint64_t> keys;
  const SidebarPresenter::IconTextureProvider provider = [&](std::uint64_t key,
                                                             const svg::RendererBitmap& bitmap) {
    EXPECT_FALSE(bitmap.empty());
    keys.push_back(key);
    return SidebarPresenter::IconTexture{kTexture, Vector2d(0.5, 0.75)};
  };
  TreeViewState state;
  constexpr char kWindow[] = "##tree_chevron_payload";
  const auto row = RenderLastTreeRowFrame(presenter, &app, state, kWindow, ImVec2(-1, -1), false,
                                          ImGuiKey_None, provider);
  ASSERT_EQ(keys.size(), 1u);
  const auto collapsedKey = keys.back();
  const auto vertices = TextureVertices(kTexture);
  ASSERT_FALSE(vertices.empty());
  for (const auto& vertex : vertices) {
    EXPECT_GE(vertex.uv.x, 0.0f);
    EXPECT_LE(vertex.uv.x, 0.5f);
    EXPECT_GE(vertex.uv.y, 0.0f);
    EXPECT_LE(vertex.uv.y, 0.75f);
  }
  const ImVec2 chevron(static_cast<float>(row.topLeft.x) - ImGui::GetFrameHeight() * 0.5f,
                       RectCenter(row).y);
  RenderLastTreeRowFrame(presenter, &app, state, kWindow, chevron, false, ImGuiKey_None, provider);
  RenderLastTreeRowFrame(presenter, &app, state, kWindow, chevron, true, ImGuiKey_None, provider);
  RenderLastTreeRowFrame(presenter, &app, state, kWindow, chevron, false, ImGuiKey_None, provider);
  EXPECT_TRUE(presenter.isTreeNodeExpandedForTesting(rootId));
  RenderLastTreeRowFrame(presenter, &app, state, kWindow, chevron, false, ImGuiKey_None, provider);
  EXPECT_NE(keys.back(), collapsedKey);
  EXPECT_FALSE(state.selectionChangedInTree);
  EXPECT_FALSE(app.hasSelection());
  EXPECT_FALSE(app.canUndo());
  EXPECT_EQ(app.document().document().source(), kInspectorSvg);
}

TEST_F(SidebarPresenterImGuiTest, WidthSpriteProviderMapsOnlyItsValidTextureAreaIntoPresetRows) {
  EditorApp app;
  SidebarPresenter presenter;
  ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, kInspectorSvg));
  constexpr ImTextureID kTexture = static_cast<ImTextureID>(0x5678);
  std::size_t spriteCalls = 0;
  const SidebarPresenter::IconTextureProvider provider = [&](std::uint64_t key,
                                                             const svg::RendererBitmap& bitmap) {
    if (key == StrokeWidthPresetTextureKey()) {
      EXPECT_EQ(bitmap.dimensions, Vector2i(256, 256));
      ++spriteCalls;
      return SidebarPresenter::IconTexture{kTexture, Vector2d(0.5, 0.75)};
    }
    return SidebarPresenter::IconTexture{};
  };
  constexpr char kWindow[] = "##width_sprite_uv_bounds";
  constexpr float kHeight = 900.0f;
  RenderInspectorFrame(presenter, &app, kWindow, ImVec2(-1, -1), false, kHeight, provider);
  const auto field = presenter.strokeWidthRectForTesting();
  ASSERT_TRUE(field.has_value());
  const auto center = RectCenter(*field);
  RenderInspectorFrame(presenter, &app, kWindow, center, false, kHeight, provider);
  RenderInspectorFrame(presenter, &app, kWindow, center, true, kHeight, provider);
  RenderInspectorFrame(presenter, &app, kWindow, center, false, kHeight, provider);
  const auto vertices = TextureVertices(kTexture);
  ASSERT_GT(spriteCalls, 0u);
  ASSERT_FALSE(vertices.empty());
  float maximumU = 0.0f;
  float maximumV = 0.0f;
  for (const auto& vertex : vertices) {
    EXPECT_GE(vertex.uv.x, 0.0f);
    EXPECT_GE(vertex.uv.y, 0.0f);
    EXPECT_LE(vertex.uv.x, 0.0625f);
    EXPECT_LE(vertex.uv.y, 0.75f);
    maximumU = std::max(maximumU, vertex.uv.x);
    maximumV = std::max(maximumV, vertex.uv.y);
  }
  EXPECT_FLOAT_EQ(maximumU, 0.0625f);
  EXPECT_FLOAT_EQ(maximumV, 0.75f);
  EXPECT_EQ(app.document().queue().size(), 0u);
  EXPECT_FALSE(app.canUndo());
}

TEST_F(SidebarPresenterImGuiTest, AuthoredRareJoinsKeepTheCompatibilityNoticeUntilChanged) {
  for (const auto& [join, notice] : std::array<std::pair<std::string_view, std::string_view>, 2>{
           {{"miter-clip", "Miter clip*"}, {"arcs", "Arcs*"}}}) {
    SCOPED_TRACE(join);
    const std::string source =
        "<svg xmlns=\"http://www.w3.org/2000/svg\"><path id=\"target\" d=\"M0 0L30 0L30 30\" "
        "stroke=\"black\" stroke-linejoin=\"" +
        std::string(join) + "\"/></svg>";
    EditorApp app;
    SidebarPresenter presenter;
    ASSERT_NO_FATAL_FAILURE(LoadAndSelectTarget(app, presenter, source));
    constexpr char kWindow[] = "##authored_join_notice";
    constexpr float kHeight = 900.0f;
    InspectorFrameCapture capture;
    EXPECT_FALSE(RenderInspectorFrame(presenter, &app, kWindow, ImVec2(-1, -1), false, kHeight, {},
                                      &capture));
    EXPECT_THAT(capture.text, HasSubstr(std::string(notice)));
    EXPECT_EQ(app.document().document().source(), source);
    EXPECT_FALSE(app.canUndo());
    EXPECT_TRUE(presenter.strokeMiterLimitVisibleForTesting());
    const auto round = presenter.strokeJoinRectForTesting(2);
    ASSERT_TRUE(round.has_value());
    RenderInspectorFrame(presenter, &app, kWindow, RectCenter(*round), false, kHeight);
    RenderInspectorFrame(presenter, &app, kWindow, RectCenter(*round), true, kHeight);
    EXPECT_TRUE(RenderInspectorFrame(presenter, &app, kWindow, RectCenter(*round), false, kHeight));
    ASSERT_TRUE(app.flushFrame());
    presenter.refreshSnapshot(app);
    RenderInspectorFrame(presenter, &app, kWindow, ImVec2(-1, -1), false, kHeight, {}, &capture);
    ASSERT_THAT(capture.text, HasSubstr("STROKE"));
    EXPECT_THAT(capture.text, Not(HasSubstr(std::string(notice))));
    EXPECT_FALSE(presenter.strokeMiterLimitVisibleForTesting());
    EXPECT_EQ(app.document().document().querySelector("#target")->getAttribute("style"),
              "stroke-linejoin: round");
    EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
    app.undo();
    app.flushFrame();
    EXPECT_EQ(app.document().document().source(), source);
  }
}

}  // namespace
}  // namespace donner::editor
