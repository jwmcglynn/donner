#include "donner/editor/StrokeMarkerPrefabs.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <string_view>

#include "donner/editor/EditorApp.h"
#include "donner/svg/SVGMarkerElement.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/properties/PropertyRegistry.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::editor {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

constexpr std::string_view kLineSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
         <path id="target" d="M10 40 L110 40" fill="none" stroke="#2468ab"/>
       </svg>)";

void SelectLine(EditorApp& app, std::string_view source) {
  ASSERT_TRUE(app.loadFromString(source));
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);
}

TEST(StrokeMarkerPrefabs, InsertsDefinitionAndStyleAsOneUndoableDocumentEdit) {
  EditorApp app;
  SelectLine(app, kLineSvg);

  ASSERT_TRUE(ApplyStrokeMarkerPrefab(app, "marker-end", StrokeMarkerPrefab::FilledArrow));
  ASSERT_TRUE(app.flushFrame());
  const auto marker = app.document().document().querySelector("#donner-marker-filled-arrow");
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(marker.has_value());
  ASSERT_TRUE(target.has_value());
  const svg::SVGMarkerElement typedMarker = marker->cast<svg::SVGMarkerElement>();
  EXPECT_EQ(typedMarker.markerWidth(), Lengthd(4));
  EXPECT_EQ(typedMarker.markerHeight(), Lengthd(4));
  EXPECT_EQ(typedMarker.refX(), Lengthd(7));
  EXPECT_EQ(typedMarker.refY(), Lengthd(4));
  EXPECT_EQ(typedMarker.orient().type(), svg::MarkerOrient::Type::AutoStartReverse);
  EXPECT_TRUE(typedMarker.viewBox().has_value());
  const auto shape = marker->firstChild();
  ASSERT_TRUE(shape.has_value());
  const RcString shapeTag = shape->tagName().name;
  EXPECT_EQ(std::string_view(shapeTag), "path");
  EXPECT_EQ(shape->getAttribute("d"), "M1 1 L7 4 L1 7 Z");
  EXPECT_EQ(shape->getAttribute("fill"), "context-stroke");
  EXPECT_EQ(
      StrokeMarkerPrefabForReference(app.document().document(), "url(#donner-marker-filled-arrow)"),
      StrokeMarkerPrefab::FilledArrow);
  EXPECT_EQ(marker->getAttribute("data-donner-prefab-marker"), "filled-arrow");
  EXPECT_EQ(marker->getAttribute("orient"), "auto-start-reverse");
  EXPECT_THAT(std::string(std::string_view(*target->getAttribute("style"))),
              HasSubstr("marker-end: url(#donner-marker-filled-arrow)"));
  EXPECT_THAT(std::string(app.document().document().source()), HasSubstr("<defs>"));
  EXPECT_THAT(std::string(app.document().document().source()), HasSubstr("context-stroke"));
  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);

  app.undo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_FALSE(app.document().document().querySelector("#donner-marker-filled-arrow").has_value());
  EXPECT_FALSE(
      app.document().document().querySelector("#target")->getAttribute("style").has_value());
  app.redo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_TRUE(app.document().document().querySelector("#donner-marker-filled-arrow").has_value());
}

TEST(StrokeMarkerPrefabs, ReusesTaggedDefinitionForStartAndEnd) {
  EditorApp app;
  SelectLine(app, kLineSvg);
  ASSERT_TRUE(ApplyStrokeMarkerPrefab(app, "marker-end", StrokeMarkerPrefab::OpenArrow));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_TRUE(ApplyStrokeMarkerPrefab(app, "marker-start", StrokeMarkerPrefab::OpenArrow));
  ASSERT_TRUE(app.flushFrame());
  const std::string source(app.document().document().source());
  EXPECT_THAT(source, HasSubstr("marker-start: url(#donner-marker-open-arrow)"));
  EXPECT_THAT(source, HasSubstr("marker-end: url(#donner-marker-open-arrow)"));
  EXPECT_EQ(source.find("<marker", source.find("<marker") + 1), std::string::npos);
}

TEST(StrokeMarkerPrefabs, NewDefinitionRendersOnTheFirstFlushedFrame) {
  constexpr std::string_view source =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="80">
           <path id="target" d="M20 40 L150 40" fill="none" stroke="#2468ab"
                 stroke-width="8"/>
         </svg>)";
  EditorApp app;
  SelectLine(app, source);
  ASSERT_TRUE(ApplyStrokeMarkerPrefab(app, "marker-end", StrokeMarkerPrefab::FilledArrow));
  ASSERT_TRUE(app.flushFrame());
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const auto markerReference = target->getComputedStyle().markerEnd.get();
  ASSERT_TRUE(markerReference.has_value());
  EXPECT_TRUE(markerReference->resolve(*target->unsafeEntityHandle().registry()).has_value());
  svg::Renderer renderer;
  renderer.draw(app.document().document());
  const svg::RendererBitmap bitmap = renderer.takeSnapshot();
  ASSERT_EQ(bitmap.dimensions, Vector2i(200, 80));
  const std::size_t alphaOffset = 30u * bitmap.rowBytes + 130u * 4u + 3u;
  ASSERT_LT(alphaOffset, bitmap.pixels.size());
  if (bitmap.pixels[alphaOffset] == 0u) {
    if (const char* outputDir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
      const std::string outputPath = std::string(outputDir) + "/marker_dynamic.png";
      EXPECT_TRUE(renderer.save(outputPath.c_str()));
    }
  }
  EXPECT_GT(bitmap.pixels[alphaOffset], 0u)
      << "filled arrow should cover (130,30), above the original 8 px line";
}

TEST(StrokeMarkerPrefabs, DynamicDefinitionKeepsSelectedCompositorTilesComplete) {
  constexpr std::string_view source =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="80">
           <path id="target" d="M20 40 L150 40" fill="none" stroke="#2468ab"
                 stroke-width="8"/>
         </svg>)";
  EditorApp app;
  SelectLine(app, source);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  svg::Renderer renderer;
  svg::compositor::CompositorConfig config;
  config.deferFirstFrameWarmup = false;
  svg::compositor::CompositorController compositor(app.document().document(), renderer, config);
  const svg::RenderViewport viewport{Vector2i(200, 80)};
  compositor.renderFrame(viewport);
  ASSERT_TRUE(compositor.promoteEntity(target->unsafeEntityHandle().entity(),
                                       svg::compositor::InteractionHint::Selection));
  compositor.renderFrame(viewport);
  ASSERT_TRUE(compositor.hasCompleteTileSetForPresentation());
  ASSERT_TRUE(ApplyStrokeMarkerPrefab(app, "marker-end", StrokeMarkerPrefab::FilledArrow));
  ASSERT_TRUE(app.flushFrame());
  compositor.renderFrame(viewport);
  EXPECT_TRUE(compositor.hasCompleteTileSetForPresentation());
}

TEST(StrokeMarkerPrefabs, PreservesExistingIdAndDefs) {
  constexpr std::string_view source =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <defs id="existing-defs"/>
           <rect id="donner-marker-dot" x="0" y="0" width="1" height="1"/>
           <path id="target" d="M10 40 L110 40" stroke="black"/>
         </svg>)";
  EditorApp app;
  SelectLine(app, source);
  ASSERT_TRUE(ApplyStrokeMarkerPrefab(app, "marker-end", StrokeMarkerPrefab::Dot));
  ASSERT_TRUE(app.flushFrame());
  const auto existing = app.document().document().querySelector("#donner-marker-dot");
  const auto prefab = app.document().document().querySelector("#donner-marker-dot-1");
  ASSERT_TRUE(existing.has_value());
  ASSERT_TRUE(prefab.has_value());
  EXPECT_EQ(std::string_view(existing->tagName().name), "rect");
  EXPECT_EQ(prefab->getAttribute("data-donner-prefab-marker"), "dot");
  EXPECT_EQ(app.document().document().querySelector("#existing-defs")->getAttribute("id"),
            "existing-defs");
  EXPECT_THAT(std::string(app.document().document().source()),
              HasSubstr("marker-end: url(#donner-marker-dot-1)"));
}

TEST(StrokeMarkerPrefabs, UnsafeTaggedIdCannotInjectCssOrCauseRepeatedDefinitions) {
  constexpr std::string_view source =
      R"marker(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <defs><marker data-donner-prefab-marker="filled-arrow"
                         id="bad); fill: red; marker-end: url(https://example.invalid/x)">
             <path d="M1 1 L7 4 L1 7 Z"/>
           </marker></defs>
           <path id="target" d="M10 40 L110 40" stroke="black"/>
         </svg>)marker";
  EditorApp app;
  SelectLine(app, source);
  ASSERT_TRUE(ApplyStrokeMarkerPrefab(app, "marker-end", StrokeMarkerPrefab::FilledArrow));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_TRUE(ApplyStrokeMarkerPrefab(app, "marker-start", StrokeMarkerPrefab::FilledArrow));
  ASSERT_TRUE(app.flushFrame());
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const auto style = target->getAttribute("style");
  ASSERT_TRUE(style.has_value());
  EXPECT_THAT(std::string(std::string_view(*style)),
              HasSubstr("marker-start: url(#donner-marker-filled-arrow)"));
  EXPECT_THAT(std::string(std::string_view(*style)),
              HasSubstr("marker-end: url(#donner-marker-filled-arrow)"));
  EXPECT_THAT(std::string(std::string_view(*style)), Not(HasSubstr("fill: red")));
  EXPECT_THAT(std::string(std::string_view(*style)), Not(HasSubstr("https://")));
  const std::string serialized(app.document().document().source());
  const std::size_t first = serialized.find("<marker");
  ASSERT_NE(first, std::string::npos);
  const std::size_t second = serialized.find("<marker", first + 1u);
  ASSERT_NE(second, std::string::npos);
  EXPECT_EQ(serialized.find("<marker", second + 1u), std::string::npos);
}

TEST(StrokeMarkerPrefabs, DuplicateIdThatResolvesToAnotherElementIsNotReused) {
  constexpr std::string_view source =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <rect id="donner-marker-filled-arrow" width="1" height="1"/>
           <defs><marker data-donner-prefab-marker="filled-arrow"
                         id="donner-marker-filled-arrow"><path d="M1 1 L7 4 L1 7 Z"/>
           </marker></defs>
           <path id="target" d="M10 40 L110 40" stroke="black"/>
         </svg>)";
  EditorApp app;
  SelectLine(app, source);
  ASSERT_TRUE(ApplyStrokeMarkerPrefab(app, "marker-end", StrokeMarkerPrefab::FilledArrow));
  ASSERT_TRUE(app.flushFrame());
  const auto created = app.document().document().querySelector("#donner-marker-filled-arrow-1");
  ASSERT_TRUE(created.has_value());
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EXPECT_THAT(std::string(app.document().document().source()),
              HasSubstr("marker-end: url(#donner-marker-filled-arrow-1)"));
}

TEST(StrokeMarkerPrefabs, TaggedMarkerWithDifferentContentIsNotReused) {
  constexpr std::string_view source =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
           <defs><marker id="donner-marker-open-arrow" data-donner-prefab-marker="open-arrow"
                         viewBox="0 0 8 8" markerWidth="4" markerHeight="4"
                         refX="7" refY="4" orient="auto-start-reverse">
             <image href="https://example.invalid/track.png" width="8" height="8"/>
           </marker></defs>
           <path id="target" d="M10 40 L110 40" stroke="black"/>
         </svg>)";
  EditorApp app;
  SelectLine(app, source);
  EXPECT_EQ(
      StrokeMarkerPrefabForReference(app.document().document(), "url(#donner-marker-open-arrow)"),
      std::nullopt);
  ASSERT_TRUE(ApplyStrokeMarkerPrefab(app, "marker-end", StrokeMarkerPrefab::OpenArrow));
  ASSERT_TRUE(app.flushFrame());
  EXPECT_TRUE(app.document().document().querySelector("#donner-marker-open-arrow-1").has_value());
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  const auto style = target->getAttribute("style");
  ASSERT_TRUE(style.has_value());
  EXPECT_THAT(std::string(std::string_view(*style)),
              HasSubstr("marker-end: url(#donner-marker-open-arrow-1)"));
}

TEST(StrokeMarkerPrefabs, RejectsUnsupportedPropertyWithoutQueuingMutation) {
  EditorApp app;
  SelectLine(app, kLineSvg);
  EXPECT_FALSE(ApplyStrokeMarkerPrefab(app, "fill", StrokeMarkerPrefab::Diamond));
  EXPECT_FALSE(app.document().hasPendingMutations());
  EXPECT_EQ(StrokeMarkerPrefabForReference(app.document().document(), "none"), std::nullopt);
}

}  // namespace
}  // namespace donner::editor
