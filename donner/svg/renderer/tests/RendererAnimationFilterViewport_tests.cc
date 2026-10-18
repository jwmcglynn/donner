#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/renderer/FilterRegionContext.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

TEST(RendererAnimationFilterViewport, MorphologyHistoryUsesExecutedIntegerRadius) {
  components::FilterGraph graph;
  FilterRegionContext context(graph, Vector2i(32, 32), Box2d::FromXYWH(0, 0, 32, 32),
                              Transform2d());
  components::FilterNode flood;
  context.recordSubregion(flood, Box2d::FromXYWH(10, 10, 8, 8));
  components::FilterNode morphology;
  morphology.inputs.emplace_back();
  morphology.primitive = components::filter_primitive::Morphology{
      .op = components::filter_primitive::Morphology::Operator::Dilate,
      .radiusX = 1.4,
      .radiusY = 1.4};
  EXPECT_THAT(context.computeNodeSubregion(morphology), testing::Eq(Box2d::FromXYWH(9, 9, 10, 10)));
}

TEST(RendererAnimationFilterViewport, ViewportOverrideRestoresDescendantLengths) {
  for (const bool removeAnimation : {false, true}) {
    SCOPED_TRACE(removeAnimation);
    const std::string prefix =
        "<svg width='32' height='16' viewBox='0 0 32 16' "
        "preserveAspectRatio='none'>";
    const std::string content = "<rect width='50%' height='16' fill='red'/>";
    auto document = instantiateSubtree(
        prefix + "<set id='a' attributeName='viewBox' to='0 0 64 16' dur='2s'/>" + content +
            "</svg>",
        parser::SVGParser::Options(), Vector2i(32, 16));
    auto expected = instantiateSubtree(prefix + content + "</svg>", parser::SVGParser::Options(),
                                       Vector2i(32, 16));
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "active-viewport-relative-length", editor::tests::PixelmatchIdentityParams());
    if (removeAnimation) {
      ASSERT_THAT(document.removeElement(*document.querySelector("#a")).applied, testing::IsTrue());
    } else {
      document.setTime(3);
    }
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        removeAnimation ? "removed-viewport-relative-length" : "expired-viewport-relative-length",
        editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationFilterViewport, SharedFilterTracksEachViewportAndTypedEdits) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  const auto filter = [](std::string_view id, std::string_view width, bool animated) {
    return "<filter id='" + std::string(id) +
           "' filterUnits='userSpaceOnUse' x='0' y='0' width='64' height='16'>"
           "<feFlood flood-color='red' x='0%' y='0' height='16' width='" +
           std::string(width) + "'>" +
           (animated ? "<animate attributeName='width' from='100%' to='8px' dur='2s'/>" : "") +
           "</feFlood></filter>";
  };
  const auto references = [](int firstWidth, std::string_view secondFilter) {
    return "<svg id='first' width='32' height='16' preserveAspectRatio='none' viewBox='0 0 " +
           std::to_string(firstWidth) +
           " 16'><rect width='64' height='16' filter='url(#f)'/></svg>"
           "<svg x='32' width='16' height='16' viewBox='8 0 16 16'>"
           "<rect width='64' height='16' filter='url(#" +
           std::string(secondFilter) + ")'/></svg>";
  };
  auto document =
      instantiateSubtree("<defs>" + filter("f", "100%", true) + "</defs>" + references(32, "f"),
                         parser::SVGParser::Options(), Vector2i(48, 16));
  document.setTime(1);
  for (const int width : {32, 64, 32}) {
    SCOPED_TRACE(width);
    document.querySelector("#first")->cast<SVGSVGElement>().setViewBox(0, 0, width, 16);
    auto expected =
        instantiateSubtree("<defs>" + filter("f", std::to_string((width + 8) / 2), false) +
                               filter("g", "12", false) + "</defs>" + references(width, "g"),
                           parser::SVGParser::Options(), Vector2i(48, 16));
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "shared-filter-referencing-viewports", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationFilterViewport, MixedImageLengthsUseReferencingViewport) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  const std::string prefix =
      "<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='24' height='16'>"
      "<feImage id='p' x='0' y='0' height='16' preserveAspectRatio='none' "
      "href='data:image/svg+xml,%3Csvg xmlns=%22http://www.w3.org/2000/svg%22 "
      "viewBox=%220 0 16 16%22%3E%3Cpath d=%22M0 0L16 16H0Z%22 "
      "fill=%22red%22/%3E%3C/svg%3E' ";
  const std::string suffix = "/></filter></defs><rect width='32' height='16' filter='url(#f)'/>";
  auto document = instantiateSubtree(
      prefix + "width='100%'" + suffix +
          "<animate href='#p' attributeName='width' from='100%' to='8px' dur='2s'/>",
      parser::SVGParser::Options(), Vector2i(32, 16));
  auto expected = instantiateSubtree(prefix + "width='20'" + suffix, parser::SVGParser::Options(),
                                     Vector2i(32, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "image-mixed-viewport-lengths", editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
