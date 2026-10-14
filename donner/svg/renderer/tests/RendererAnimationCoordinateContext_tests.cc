#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <tuple>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGFEFloodElement.h"
#include "donner/svg/SVGMarkerElement.h"
#include "donner/svg/SVGPatternElement.h"
#include "donner/svg/SVGTextPathElement.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/renderer/FilterRegionContext.h"
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

TEST(RendererAnimationCoordinateContext, ContextualMarkerStackPreservesTypedBaseAndSeeking) {
  const std::string suffix =
      " markerHeight='8' markerUnits='userSpaceOnUse' viewBox='0 0 10 8' "
      "preserveAspectRatio='none'>"
      "<rect width='10' height='8'/></marker></defs>"
      "<path d='M0 4H4' marker-end='url(#p)'/>";
  auto document = instantiateSubtree(
      "<defs><marker id='p' markerWidth='4'" + suffix +
          "<set href='#p' attributeName='markerWidth' to='8' dur='2s'/>"
          "<animate href='#p' attributeName='markerWidth' from='0' to='4' additive='sum' dur='2s'/>"
          "<animate href='#p' attributeName='markerWidth' to='12' dur='2s'/>",
      parser::SVGParser::Options(), Vector2i(20, 16));
  for (const double time : {1.0, 3.0, 1.0}) {
    SCOPED_TRACE(time);
    auto expected = instantiateSubtree(
        std::string("<defs><marker id='p' markerWidth='") + (time == 1 ? "11'" : "4'") + suffix,
        parser::SVGParser::Options(), Vector2i(20, 16));
    document.setTime(time);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        time == 1 ? "contextual-marker-stack" : "contextual-marker-restore",
        editor::tests::PixelmatchIdentityParams());
  }
  document.querySelector("#p")->cast<SVGMarkerElement>().setMarkerWidth(Lengthd(6));
  document.setTime(3);
  auto edited = instantiateSubtree("<defs><marker id='p' markerWidth='6'" + suffix,
                                   parser::SVGParser::Options(), Vector2i(20, 16));
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(edited),
      "contextual-marker-typed-edit", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationCoordinateContext, RepeatedMarkersShareOneContextualWorkAllowance) {
  std::string values = "8";
  for (int i = 1; i < 512; ++i) {
    values += ";8";
  }
  std::string source =
      "<defs><marker id='p' markerWidth='3' markerHeight='3'>"
      "<rect width='3' height='3'/><animate attributeName='markerWidth' values='" +
      values + "' dur='2s'/></marker></defs>";
  for (int i = 0; i < 600; ++i) {
    source += "<path d='M0 8H8' marker-end='url(#p)'/>";
  }
  auto document = instantiateSubtree(source, parser::SVGParser::Options(), Vector2i(16, 16));
  document.setTime(1);
  (void)RenderDocumentWithActiveBackend(document);
  std::size_t animated = 0;
  std::size_t base = 0;
  for (const auto entity : document.registry().view<components::RenderingInstanceComponent>()) {
    const auto& instance = document.registry().get<components::RenderingInstanceComponent>(entity);
    if (instance.markerEnd && instance.markerEnd->animatedLengths) {
      const double width = instance.markerEnd->animatedLengths->width;
      EXPECT_THAT(width, testing::AnyOf(testing::Eq(3), testing::Eq(8)));
      animated += width == 8;
      base += width == 3;
    }
  }
  EXPECT_THAT(animated, testing::Gt(0u));
  EXPECT_THAT(base, testing::Gt(0u));
  EXPECT_THAT(animated + base, testing::Eq(600u));
  EXPECT_THAT(document.registry()
                  .ctx()
                  .get<components::AnimationResourceBudget>()
                  .remainingContextualWork(),
              testing::Eq(0u));
}

TEST(RendererAnimationCoordinateContext, MarkerReferenceKeywordsAnimateAndRestore) {
  for (const auto& [attribute, keyword, number] :
       {std::tuple{"refX", "center", "50%"}, std::tuple{"refX", "right", "100%"},
        std::tuple{"refY", "bottom", "100%"}, std::tuple{"refY", "top", "0%"}}) {
    SCOPED_TRACE(attribute);
    SCOPED_TRACE(keyword);
    const std::string suffix =
        " markerWidth='10' markerHeight='10' markerUnits='userSpaceOnUse' viewBox='0 0 10 10'>"
        "<rect width='10' height='10'/></marker></defs><path d='M0 12H12' marker-end='url(#p)'/>";
    auto document =
        instantiateSubtree("<defs><marker id='p'" + suffix + "<set href='#p' attributeName='" +
                               attribute + "' to='" + keyword + "' dur='1s'/>",
                           parser::SVGParser::Options(), Vector2i(24, 24));
    auto expected = instantiateSubtree(
        std::string("<defs><marker id='p' ") + attribute + "='" + number + "'" + suffix,
        parser::SVGParser::Options(), Vector2i(24, 24));
    document.setTime(0.5);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected),
                                         std::string("marker-keyword-") + attribute + '-' + keyword,
                                         editor::tests::PixelmatchIdentityParams());
    document.setTime(2);
    auto base = instantiateSubtree("<defs><marker id='p'" + suffix, parser::SVGParser::Options(),
                                   Vector2i(24, 24));
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(base),
        std::string("marker-keyword-restored-") + attribute + '-' + keyword,
        editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationCoordinateContext, ContextualRecipeAdmissionCountsReferenceMetadata) {
  const std::string id(1200, 'a');
  const std::string source = "<defs><marker id='" + id + "'/></defs><animate href='#" + id +
                             "' attributeName='markerWidth' to='8' dur='2s'/>";
  auto document = instantiateSubtree(source, parser::SVGParser::Options(), Vector2i(16, 16));
  document.setTime(1);
  (void)RenderDocumentWithActiveBackend(document);
  const auto target = document.querySelector("#" + id);
  ASSERT_THAT(target, testing::Ne(std::nullopt));
  const auto& values = target->entityHandle().get<components::AnimatedValuesComponent>();
  ASSERT_THAT(values.contextualLengths, testing::Contains(testing::Key("markerWidth")));
  const auto& recipe = *values.contextualLengths.at("markerWidth");
  ASSERT_THAT(recipe.samples, testing::SizeIs(1));
  const auto& declaration =
      std::get<components::AnimateValueComponent>(recipe.samples[0].declaration);
  ASSERT_THAT(declaration.href, testing::Ne(std::nullopt));
  EXPECT_THAT(recipe.retainedBytes,
              testing::Ge(sizeof(components::ContextualLengthSample) + declaration.href->size() +
                          declaration.attributeName.size() + declaration.to->size()));
  EXPECT_THAT(document.registry().ctx().get<components::AnimationResourceBudget>().sourceBytes(),
              testing::Ge(declaration.href->size()));
}

#ifdef DONNER_TEXT_ENABLED
TEST(RendererAnimationCoordinateContext, ShadowTextPathRecipeClearsAfterSourceRemoval) {
  auto document = instantiateSubtree(
      "<defs><g id='label'><text font-size='12'><textPath path='M0 20H40'>H"
      "<animate id='offset' attributeName='startOffset' from='50%' to='0' dur='2s'/>"
      "</textPath></text></g></defs><use href='#label' x='4'/>",
      parser::SVGParser::Options(), Vector2i(64, 32));
  auto expected = instantiateSubtree(
      "<g transform='translate(4 0)'><text font-size='12'>"
      "<textPath path='M0 20H40' startOffset='10'>H</textPath></text></g>",
      parser::SVGParser::Options(), Vector2i(64, 32));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "shadow-text-offset", editor::tests::PixelmatchIdentityParams());
  const auto animation = document.querySelector("#offset");
  ASSERT_THAT(animation, testing::Ne(std::nullopt));
  ASSERT_THAT(document.removeElement(*animation).applied, testing::IsTrue());
  auto base = instantiateSubtree(
      "<g transform='translate(4 0)'><text font-size='12'>"
      "<textPath path='M0 20H40'>H</textPath></text></g>",
      parser::SVGParser::Options(), Vector2i(64, 32));
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(base),
      "shadow-text-offset-removed", editor::tests::PixelmatchIdentityParams());
}
#endif

TEST(RendererAnimationCoordinateContext, BoundingBoxPrimitiveLengthsPreservePositionsAndDeltas) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  struct Case {
    const char* name;
    const char* initial;
    const char* animation;
    const char* expected;
    double time = 1;
  };
  for (const auto& item : {
           Case{"width-absolute", "x='0' width='0.5'",
                "attributeName='width' from='100%' to='4px' dur='2s'", "x='0' width='6px'"},
           Case{"position-absolute", "x='0' width='0.5'",
                "attributeName='x' from='0%' to='8px' dur='2s'", "x='6px' width='0.5'"},
           Case{"position-additive", "x='0.25' width='0.5'",
                "attributeName='x' from='0px' to='4px' additive='sum' dur='2s'",
                "x='8px' width='0.5'"},
           Case{"position-by", "x='0' width='0.5'", "attributeName='x' from='0%' by='4px' dur='2s'",
                "x='6px' width='0.5'"},
           Case{"position-accumulated", "x='0' width='0.5'",
                "attributeName='x' from='0%' to='0.25' dur='1s' repeatCount='2' accumulate='sum'",
                "x='7px' width='0.5'", 1.5},
       }) {
    SCOPED_TRACE(item.name);
    const std::string prefix =
        "<defs><filter id='f' filterUnits='userSpaceOnUse' primitiveUnits='objectBoundingBox' "
        "x='0' y='0' width='16' height='16'><feFlood id='p' flood-color='red' y='0' height='1' ";
    const std::string suffix =
        "/></filter></defs><rect x='4' y='4' width='8' height='8' filter='url(#f)'/>";
    auto document = instantiateSubtree(
        prefix + item.initial + suffix + "<animate href='#p' " + item.animation + "/>",
        parser::SVGParser::Options(), Vector2i(16, 16));
    auto expected = instantiateSubtree(prefix + item.expected + suffix,
                                       parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(item.time);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected), item.name,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationCoordinateContext, AdditivePrimitiveAccumulationKeepsDisplacements) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  const std::string prefix =
      "<defs><filter id='f' filterUnits='userSpaceOnUse' primitiveUnits='objectBoundingBox' "
      "x='0' y='0' width='32' height='16'><feFlood flood-color='red' y='0' height='1' ";
  const std::string suffix =
      "/></filter></defs><rect x='4' y='4' width='8' height='8' filter='url(#f)'/>";
  auto document =
      instantiateSubtree(prefix + "id='p' x='0' width='0.5'" + suffix +
                             "<animate href='#p' attributeName='x' values='0;0;10px' dur='2s' "
                             "repeatCount='3' additive='sum' accumulate='sum'/>",
                         parser::SVGParser::Options(), Vector2i(32, 16));
  auto expected = instantiateSubtree(prefix + "x='14px' width='0.5'" + suffix,
                                     parser::SVGParser::Options(), Vector2i(32, 16));
  document.setTime(2.5);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "primitive-additive-accumulated-displacement", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationCoordinateContext, SvgFilterImageRasterizesAtAnimatedSize) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  const std::string prefix =
      "<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='32' height='16'>"
      "<feImage id='p' x='0' y='0' height='16' preserveAspectRatio='none' "
      "href='data:image/svg+xml,%3Csvg xmlns=%22http://www.w3.org/2000/svg%22 "
      "viewBox=%220 0 16 16%22%3E%3Cpath d=%22M0 0L16 16H0Z%22 "
      "fill=%22red%22/%3E%3C/svg%3E' ";
  const std::string suffix = "/></filter></defs><rect width='32' height='16' filter='url(#f)'/>";
  auto document =
      instantiateSubtree(prefix + "width='4'" + suffix +
                             "<animate href='#p' attributeName='width' from='4' to='28' dur='2s'/>",
                         parser::SVGParser::Options(), Vector2i(32, 16));
  auto expected = instantiateSubtree(prefix + "width='16'" + suffix, parser::SVGParser::Options(),
                                     Vector2i(32, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "svg-filter-image-animated-raster-size", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationCoordinateContext, ScaledFilterHistoryRetainsUserBounds) {
  components::FilterGraph graph;
  FilterRegionContext context(graph, Vector2i(64, 64), Box2d::FromXYWH(0, 0, 32, 32),
                              Transform2d::Scale(2));
  components::FilterNode flood;
  flood.primitive = components::filter_primitive::Flood{};
  context.recordSubregion(flood, Box2d::FromXYWH(10, 10, 8, 8));
  components::FilterNode blur;
  blur.inputs.emplace_back();
  blur.primitive = components::filter_primitive::GaussianBlur{1, 1};
  EXPECT_THAT(context.computeNodeSubregion(blur), testing::Eq(Box2d::FromXYWH(7, 7, 14, 14)));
}

TEST(RendererAnimationCoordinateContext, PrimitiveUnderlyingBlurBoundsUseUserCoordinates) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  const std::string prefix =
      "<svg width='64' height='64' viewBox='0 0 32 32'><defs>"
      "<filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='32' height='32'>"
      "<feFlood x='10' y='10' width='8' height='8' flood-color='red'/>"
      "<feGaussianBlur stdDeviation='1'/><feOffset id='p' ";
  const std::string suffix =
      "/></filter></defs><rect width='32' height='32' filter='url(#f)'/></svg>";
  auto document = instantiateSubtree(
      prefix + suffix + "<animate href='#p' attributeName='width' to='6' dur='2s'/>",
      parser::SVGParser::Options(), Vector2i(64, 64));
  auto expected = instantiateSubtree(prefix + "x='7' y='7' width='10' height='14'" + suffix,
                                     parser::SVGParser::Options(), Vector2i(64, 64));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "primitive-scaled-blur-underlying-region", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationCoordinateContext, PrimitiveUnderlyingRegionUsesInputBounds) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  const std::string prefix =
      "<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='16' height='16'>"
      "<feFlood x='2' y='0' width='8' height='16' flood-color='red' result='paint'/>"
      "<feOffset id='p' in='paint' dx='0' ";
  const std::string suffix = "/></filter></defs><rect width='16' height='16' filter='url(#f)'/>";
  auto document = instantiateSubtree(
      prefix + suffix + "<animate href='#p' attributeName='width' to='4' dur='2s'/>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  auto expected = instantiateSubtree(prefix + "x='2' y='0' width='6' height='16'" + suffix,
                                     parser::SVGParser::Options(), Vector2i(16, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "primitive-input-region-base", editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
