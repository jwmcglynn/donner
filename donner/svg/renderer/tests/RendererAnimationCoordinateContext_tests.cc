#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGPatternElement.h"
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

}  // namespace
}  // namespace donner::svg
