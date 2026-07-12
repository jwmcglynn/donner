#include "donner/editor/SidebarPresenter.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "donner/base/MathUtils.h"
#include "donner/base/Transform.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/ImGuiInternalIncludes.h"
#include "donner/svg/DocumentState.h"

namespace donner::editor {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Pair;

constexpr std::string_view kInspectorSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
         <rect x="10" y="20" width="100" height="50" fill="red" id="target"/>
         <rect id="peer" x="0" y="0" width="10" height="10"/>
       </svg>)";

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
                                   bool mouseDown = false) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400, 300);
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(0, mouseDown);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_Always);
    ImGui::Begin(windowName, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    const bool queuedMutation = presenter.renderInspector(app, ViewportState{});
    ImGui::End();
    ImGui::Render();
    return queuedMutation;
  }
};

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
  int nonEmptyBitmaps = 0;
  int retinaBitmaps = 0;
  const SidebarPresenter::IconTextureProvider iconTextureProvider =
      [&](std::uint64_t, const svg::RendererBitmap& bitmap) {
        ++providerCalls;
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

  EXPECT_EQ(providerCalls, 4)
      << "the path operation UI should request Union, Intersect, Subtract Front, and Exclude "
         "icon textures";
  EXPECT_EQ(nonEmptyBitmaps, providerCalls)
      << "path operation buttons must receive Donner-rendered Bootstrap SVG bitmaps";
  EXPECT_EQ(retinaBitmaps, providerCalls)
      << "path operation icons are drawn at 18 logical px and must be rasterized at 2x or "
         "higher before ImGui scales them";
  EXPECT_EQ(imageQuads, 4) << "path operation buttons should render as image quads";
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
  const ImDrawData* collapsedDrawData = ImGui::GetDrawData();
  ASSERT_NE(collapsedDrawData, nullptr);
  const int collapsedVertices = collapsedDrawData->TotalVtxCount;

  // Opening the disclosure renders the six raw matrix drag cells: strictly
  // more geometry, still no queued mutation and no undo entry.
  EXPECT_FALSE(RenderInspectorFrameWithMatrixOpen(presenter, &app, kWindowName));
  const ImDrawData* openDrawData = ImGui::GetDrawData();
  ASSERT_NE(openDrawData, nullptr);
  EXPECT_GT(openDrawData->TotalVtxCount, collapsedVertices)
      << "The open matrix disclosure must draw the a-f component cells.";
  EXPECT_FALSE(app.canUndo()) << "Rendering the matrix cells must not record undo entries.";

  // An in-progress matrix edit displays the edit-buffer value for its cell.
  presenter.beginTransformEditForTesting(app, SidebarPresenter::TransformField::Matrix,
                                         /*matrixIndex=*/2);
  EXPECT_FALSE(RenderInspectorFrameWithMatrixOpen(presenter, &app, kWindowName));
  presenter.commitTransformEditForTesting(app);
  EXPECT_FALSE(presenter.hasTransformEditForTesting());

  // Busy frame (no live app): the cells render disabled and stay inert.
  EXPECT_FALSE(RenderInspectorFrameWithMatrixOpen(presenter, nullptr, kWindowName));
  const ImDrawData* busyDrawData = ImGui::GetDrawData();
  ASSERT_NE(busyDrawData, nullptr);
  EXPECT_GT(busyDrawData->TotalVtxCount, collapsedVertices);
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
  int providerCalls = 0;
  const SidebarPresenter::IconTextureProvider iconTextureProvider =
      [&](std::uint64_t, const svg::RendererBitmap&) {
        ++providerCalls;
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
  EXPECT_EQ(providerCalls, 4)
      << "Disabled path-operation buttons must still request all four icon textures.";
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
    applied = RenderInspectorFrame(presenter, &app, kWindowName, dragged, /*mouseDown=*/true) ||
              applied;
  }
  EXPECT_TRUE(applied) << "Dragging the X field must queue a live transform mutation.";

  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, kWindowName, dragged, /*mouseDown=*/false));
  EXPECT_FALSE(presenter.hasTransformEditForTesting())
      << "Releasing the drag must commit the transform edit.";
  EXPECT_TRUE(app.canUndo()) << "The committed drag edit must be undoable.";
}

}  // namespace
}  // namespace donner::editor
