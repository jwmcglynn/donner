#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGFEFloodElement.h"
#include "donner/svg/SVGPatternElement.h"
#include "donner/svg/SVGTextPathElement.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

TEST(RendererAnimationCoordinateContext, GradientUsesUnitsSampledAtTheSameTime) {
  for (const bool inherited : {false, true}) {
    SCOPED_TRACE(inherited);
    const std::string prefix =
        inherited ? "<defs><linearGradient id='base'/><linearGradient id='g' href='#base'"
                  : "<defs><linearGradient id='g'";
    const std::string suffix =
        "><stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient>"
        "</defs><rect width='16' height='16' fill='url(#g)'/>";
    const std::string animations =
        "<animate href='#g' attributeName='x2' from='100%' to='8' dur='2s'/>"
        "<set href='" +
        std::string(inherited ? "#base" : "#g") +
        "' attributeName='gradientUnits' to='userSpaceOnUse' dur='2s'/>";
    auto document = instantiateSubtree(prefix + suffix + animations, parser::SVGParser::Options(),
                                       Vector2i(16, 16));
    auto expected = instantiateSubtree(prefix + " gradientUnits='userSpaceOnUse' x2='12'" + suffix,
                                       parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        inherited ? "inherited-sampled-gradient-units" : "sampled-gradient-units",
        editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationCoordinateContext, GradientUsesSampledReferenceUnits) {
  const std::string prefix =
      "<defs><linearGradient id='a'/><linearGradient id='b' gradientUnits='userSpaceOnUse'/>"
      "<linearGradient id='g'";
  const std::string suffix =
      "><stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient>"
      "</defs><rect width='16' height='16' fill='url(#g)'/>";
  auto document =
      instantiateSubtree(prefix + " href='#a'" + suffix +
                             "<animate href='#g' attributeName='x2' from='100%' to='8' dur='2s'/>"
                             "<set href='#g' attributeName='href' to='#b' dur='2s'/>",
                         parser::SVGParser::Options(), Vector2i(16, 16));
  auto expected = instantiateSubtree(prefix + " href='#b' x2='12'" + suffix,
                                     parser::SVGParser::Options(), Vector2i(16, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "sampled-gradient-reference-units", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationCoordinateContext, MaskMixedLengthsUseTheSelectedUnits) {
  for (const bool userSpace : {false, true}) {
    SCOPED_TRACE(userSpace);
    const std::string prefix =
        std::string("<defs><mask id='p'") + (userSpace ? " maskUnits='userSpaceOnUse'" : "");
    const std::string suffix =
        "><rect width='16' height='16' fill='white'/></mask></defs>"
        "<rect x='4' y='4' width='8' height='8' mask='url(#p)'/>";
    auto document = instantiateSubtree(
        prefix + suffix + "<animate href='#p' attributeName='width' to='0.8' dur='2s'/>",
        parser::SVGParser::Options(), Vector2i(16, 16));
    auto expected = instantiateSubtree(prefix + (userSpace ? " width='10'" : " width='1'") + suffix,
                                       parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected),
                                         userSpace ? "mask-user-units" : "mask-bounding-box-units",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationCoordinateContext, PatternUsesInheritedBaseAndSampledUnits) {
  for (const bool sampledUnits : {false, true}) {
    SCOPED_TRACE(sampledUnits);
    const std::string prefix =
        "<defs><pattern id='base' width='100%' height='1'/>"
        "<pattern id='p' href='#base'";
    const std::string suffix =
        "><rect width='2' height='16'/></pattern></defs>"
        "<rect width='16' height='16' fill='url(#p)'/>";
    auto document = instantiateSubtree(
        prefix + suffix + "<animate href='#p' attributeName='width' to='0.5' dur='2s'/>" +
            (sampledUnits ? "<set href='#base' attributeName='patternUnits' "
                            "to='userSpaceOnUse' dur='2s'/>"
                          : ""),
        parser::SVGParser::Options(), Vector2i(16, 16));
    auto expected = instantiateSubtree(
        prefix + (sampledUnits ? " patternUnits='userSpaceOnUse' width='8.25'" : " width='0.75'") +
            suffix,
        parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        sampledUnits ? "pattern-sampled-units" : "pattern-inherited-width",
        editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationCoordinateContext, FilterUsesDefaultAndInheritedRegionLengths) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  for (const bool inherited : {false, true}) {
    SCOPED_TRACE(inherited);
    const std::string prefix =
        std::string("<defs>") + (inherited
                                     ? "<filter id='base' width='80%'/><filter id='f' href='#base'"
                                     : "<filter id='f'");
    const std::string suffix =
        "><feOffset/></filter></defs><rect width='16' height='16' filter='url(#f)'/>";
    auto document = instantiateSubtree(
        prefix + suffix + "<animate href='#f' attributeName='width' to='0.8' dur='2s'/>",
        parser::SVGParser::Options(), Vector2i(16, 16));
    auto expected =
        instantiateSubtree(prefix + (inherited ? " width='0.8'" : " width='1'") + suffix,
                           parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        inherited ? "filter-inherited-region" : "filter-default-region",
        editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationCoordinateContext, PatternBaseUsesStylesheetAndTypedOverridePrecedence) {
  for (const bool typedOverride : {false, true}) {
    SCOPED_TRACE(typedOverride);
    const std::string ending =
        " height='16'><rect width='2' height='16'/></pattern></defs>"
        "<rect width='16' height='16' fill='url(#p)'/>";
    auto document = instantiateSubtree(
        "<style>#p {width:8}</style><defs><pattern id='p' patternUnits='userSpaceOnUse'" + ending +
            "<animate href='#p' attributeName='width' to='12' dur='2s'/>",
        parser::SVGParser::Options(), Vector2i(16, 16));
    if (typedOverride) {
      document.querySelector("#p")->cast<SVGPatternElement>().setWidth(Lengthd(10));
    }
    auto expected = instantiateSubtree(
        std::string("<defs><pattern id='p' patternUnits='userSpaceOnUse' width='") +
            (typedOverride ? "11'" : "10'") + ending,
        parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected),
                                         typedOverride ? "pattern-typed-base" : "pattern-css-base",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

#ifdef DONNER_TEXT_ENABLED
TEST(RendererAnimationCoordinateContext, TextPathToOnlyUsesTypedOffset) {
  const std::string prefix = "<text font-size='12'><textPath id='p' path='M0 14H64'";
  const std::string suffix = ">H</textPath></text>";
  auto document =
      instantiateSubtree(prefix + " startOffset='0'" + suffix +
                             "<animate href='#p' attributeName='startOffset' to='16' dur='2s'/>",
                         parser::SVGParser::Options(), Vector2i(64, 16));
  document.querySelector("#p")->cast<SVGTextPathElement>().setStartOffset(Lengthd(8));
  auto expected = instantiateSubtree(prefix + " startOffset='12'" + suffix,
                                     parser::SVGParser::Options(), Vector2i(64, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "text-path-typed-offset", editor::tests::PixelmatchIdentityParams());
  auto restored = instantiateSubtree(prefix + " startOffset='8'" + suffix,
                                     parser::SVGParser::Options(), Vector2i(64, 16));
  document.setTime(3);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(restored),
      "text-path-typed-offset-restored", editor::tests::PixelmatchIdentityParams());
}
#endif

TEST(RendererAnimationCoordinateContext, FilterPrimitiveToOnlyUsesTypedRegion) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  const std::string prefix =
      "<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='16' height='16'>"
      "<feFlood id='p' flood-color='red' x='0' y='0' height='16'";
  const std::string suffix = "/></filter></defs><rect width='16' height='16' filter='url(#f)'/>";
  auto document =
      instantiateSubtree(prefix + " width='4'" + suffix +
                             "<animate href='#p' attributeName='width' to='12' dur='2s'/>",
                         parser::SVGParser::Options(), Vector2i(16, 16));
  document.querySelector("#p")->cast<SVGFEFloodElement>().setWidth(Lengthd(8));
  auto expected = instantiateSubtree(prefix + " width='10'" + suffix, parser::SVGParser::Options(),
                                     Vector2i(16, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "filter-primitive-typed-region", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationCoordinateContext, FilterPrimitiveUsesInheritedAndSampledUnits) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  for (const bool sampled : {false, true}) {
    SCOPED_TRACE(sampled);
    const std::string prefix =
        std::string("<defs><filter id='base' primitiveUnits='") +
        (sampled ? "userSpaceOnUse" : "objectBoundingBox") +
        "'/><filter id='f' href='#base'><feFlood id='p' flood-color='red' x='0' y='0' height='1'";
    const std::string suffix = "/></filter></defs><rect width='16' height='16' filter='url(#f)'/>";
    auto document = instantiateSubtree(
        prefix + suffix +
            "<animate href='#p' attributeName='width' from='100%' to='0.5' dur='2s'/>" +
            (sampled ? "<set href='#base' attributeName='primitiveUnits' to='objectBoundingBox' "
                       "dur='2s'/>"
                     : ""),
        parser::SVGParser::Options(), Vector2i(16, 16));
    auto expected = instantiateSubtree(
        "<defs><filter id='f' primitiveUnits='objectBoundingBox'><feFlood flood-color='red' "
        "x='0' y='0' height='1' width='0.75'" +
            suffix,
        parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        sampled ? "primitive-sampled-units" : "primitive-inherited-units",
        editor::tests::PixelmatchIdentityParams());
  }
}

#ifdef DONNER_TEXT_ENABLED
TEST(RendererAnimationCoordinateContext, MixedTextPathOffsetUsesFinalPathLength) {
  for (const bool animatedPath : {false, true}) {
    SCOPED_TRACE(animatedPath);
    const std::string geometry =
        animatedPath ? "<defs><path id='curve' d='M0 20H40'><animate attributeName='d' from='M0 "
                       "20H40' to='M0 20H80' dur='2s'/></path></defs>"
                     : "";
    auto document = instantiateSubtree(
        geometry + "<text font-size='12'><textPath id='label' " +
            (animatedPath ? "href='#curve'" : "path='M0 20H40'") +
            ">H<animate attributeName='startOffset' from='50%' to='0' dur='2s'/></textPath></text>",
        parser::SVGParser::Options(), Vector2i(100, 40));
    auto expected =
        instantiateSubtree(std::string("<text font-size='12'><textPath path='M0 20H") +
                               (animatedPath ? "60' startOffset='15'" : "40' startOffset='10'") +
                               ">H</textPath></text>",
                           parser::SVGParser::Options(), Vector2i(100, 40));
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        animatedPath ? "text-offset-final-animated-path" : "text-offset-inline-path",
        editor::tests::PixelmatchIdentityParams());
  }
}
#endif

TEST(RendererAnimationCoordinateContext, SharedMarkerMixedLengthsUseEachReferencingViewport) {
  const std::string marker =
      " markerHeight='10' markerUnits='userSpaceOnUse' viewBox='0 0 10 10' "
      "preserveAspectRatio='none'>"
      "<rect width='10' height='10'/>";
  const std::string shapes =
      "<path d='M0 10H20' marker-end='url(#a)'/>"
      "<svg y='40' width='50' height='30' viewBox='0 0 50 30'>"
      "<path d='M0 10H20' marker-end='url(#b)'/></svg>";
  auto document =
      instantiateSubtree("<defs><marker id='a'" + marker +
                             "<animate attributeName='markerWidth' from='50%' to='10' dur='2s'/>"
                             "</marker></defs><path d='M0 10H20' marker-end='url(#a)'/>"
                             "<svg y='40' width='50' height='30' viewBox='0 0 50 30'>"
                             "<path d='M0 10H20' marker-end='url(#a)'/></svg>",
                         parser::SVGParser::Options(), Vector2i(100, 80));
  auto expected = instantiateSubtree("<defs><marker id='a' markerWidth='30'" + marker +
                                         "</marker>"
                                         "<marker id='b' markerWidth='17.5'" +
                                         marker + "</marker></defs>" + shapes,
                                     parser::SVGParser::Options(), Vector2i(100, 80));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "marker-per-reference-viewport", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationCoordinateContext, InheritedPrimitiveUsesReferencingFilterUnits) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  auto document = instantiateSubtree(
      "<defs><filter id='base' primitiveUnits='userSpaceOnUse'>"
      "<feFlood x='0' y='0' height='1' flood-color='red'>"
      "<animate attributeName='width' from='100%' to='0.5' dur='2s'/></feFlood></filter>"
      "<filter id='f' href='#base' primitiveUnits='objectBoundingBox'/></defs>"
      "<rect width='16' height='16' filter='url(#f)'/>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  auto expected = instantiateSubtree(
      "<defs><filter id='f' primitiveUnits='objectBoundingBox'>"
      "<feFlood x='0' y='0' width='0.75' height='1' flood-color='red'/></filter></defs>"
      "<rect width='16' height='16' filter='url(#f)'/>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "primitive-reference-context", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationCoordinateContext, PrimitiveToOnlyStartsAtDefaultSubregion) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  const std::string prefix =
      "<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='16' height='16'>"
      "<feFlood id='p' x='0' y='0' flood-color='red'";
  const std::string suffix = "/></filter></defs><rect width='16' height='16' filter='url(#f)'/>";
  auto document = instantiateSubtree(
      prefix + suffix + "<animate href='#p' attributeName='width' to='8' dur='2s'/>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  auto expected = instantiateSubtree(prefix + " width='12'" + suffix, parser::SVGParser::Options(),
                                     Vector2i(16, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "primitive-default-subregion", editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
