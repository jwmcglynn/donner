#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleComponent.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg {
namespace {

TEST(RendererAnimationStyleBase, ScalarPercentagesIgnoreViewportSize) {
  struct Case {
    const char* attribute;
    const char* source;
  };
  for (const Case item : {
           Case{"opacity", "<rect id='r' width='16' height='16' fill='red'/>"},
           Case{"fill-opacity", "<rect id='r' width='16' height='16' fill='red'/>"},
           Case{"stroke-opacity", "<path id='r' d='M0 8H16' stroke='red' stroke-width='8'/>"},
           Case{"stop-opacity",
                "<defs><linearGradient id='g'><stop id='r' "
                "stop-color='red'/></linearGradient></defs><rect width='16' height='16' "
                "fill='url(#g)'/>"},
           Case{"offset",
                "<defs><linearGradient id='g'><stop id='r' stop-color='red'/><stop offset='1' "
                "stop-color='blue'/></linearGradient></defs><rect width='16' height='16' "
                "fill='url(#g)'/>"},
           Case{"flood-opacity",
                "<defs><filter id='f'><feFlood id='r' flood-color='red'/></filter></defs><rect "
                "width='16' height='16' filter='url(#f)'/>"},
       }) {
    for (const int size : {16, 64}) {
      SCOPED_TRACE(item.attribute);
      SCOPED_TRACE(size);
      const std::string animation = "<animate href='#r' attributeName='" +
                                    std::string(item.attribute) + "' from='100%' to='0' dur='2s'/>";
      auto document = instantiateSubtree(std::string(item.source) + animation,
                                         parser::SVGParser::Options(), Vector2i(size, size));
      std::string expectedSource = item.source;
      expectedSource.insert(expectedSource.find("id='r'"), std::string(item.attribute) + "='0.5' ");
      auto expected =
          instantiateSubtree(expectedSource, parser::SVGParser::Options(), Vector2i(size, size));
      document.setTime(1);
      editor::tests::CompareBitmapToBitmap(
          RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
          "scalar-percentage-context", editor::tests::PixelmatchIdentityParams());
    }
  }
}

TEST(RendererAnimationStyleBase, ToOnlyResourceOpacityStartsAtOne) {
  for (const bool flood : {false, true}) {
    SCOPED_TRACE(flood);
    const std::string attribute = flood ? "flood-opacity" : "stop-opacity";
    const std::string source =
        flood ? "<defs><filter id='f'><feFlood id='r' flood-color='red'/></filter></defs><rect "
                "width='16' height='16' filter='url(#f)'/>"
              : "<defs><linearGradient id='g'><stop id='r' "
                "stop-color='red'/></linearGradient></defs><rect width='16' height='16' "
                "fill='url(#g)'/>";
    auto document = instantiateSubtree(
        source + "<animate href='#r' attributeName='" + attribute + "' to='0' dur='2s'/>",
        parser::SVGParser::Options(), Vector2i(16, 16));
    std::string expectedSource = source;
    expectedSource.insert(expectedSource.find("id='r'"), attribute + "='0.5' ");
    auto expected =
        instantiateSubtree(expectedSource, parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "resource-opacity-default", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationStyleBase, NumericOperandsCannotLaunderLengthUnits) {
  struct Case {
    const char* attribute;
    const char* source;
  };
  for (const Case item : {
           Case{"stroke-miterlimit",
                "<path id='r' d='M4 28L16 4L28 28' fill='none' stroke='red' stroke-width='6' "
                "stroke-miterlimit='4'/>"},
           Case{"slope",
                "<defs><filter id='f'><feComponentTransfer><feFuncR id='r' "
                "type='linear'/></feComponentTransfer></filter></defs><rect x='8' y='8' width='16' "
                "height='16' fill='#800000' filter='url(#f)'/>"},
           Case{"stdDeviation",
                "<defs><filter id='f'><feGaussianBlur id='r'/></filter></defs><rect x='8' y='8' "
                "width='16' height='16' fill='red' filter='url(#f)'/>"},
       }) {
    SCOPED_TRACE(item.attribute);
    auto document =
        instantiateSubtree(std::string(item.source) + "<animate href='#r' attributeName='" +
                               item.attribute + "' from='1px' to='3' dur='2s'/>",
                           parser::SVGParser::Options(), Vector2i(32, 32));
    auto expected = instantiateSubtree(item.source, parser::SVGParser::Options(), Vector2i(32, 32));
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "numeric-unit-rejection", editor::tests::PixelmatchIdentityParams());
  }
}

#ifdef DONNER_TEXT_ENABLED
TEST(RendererAnimationStyleBase, FontSizeUsesTheSampledParentFontContext) {
  for (const bool animatedParent : {false, true}) {
    for (const std::string_view from : {"200%", "2em"}) {
      SCOPED_TRACE(animatedParent);
      SCOPED_TRACE(from);
      const std::string source =
          "<g id='parent' font-size='12'><text id='label' x='0' y='30' font-size='24'>H</text></g>";
      auto document = instantiateSubtree(
          source + "<animate href='#label' attributeName='font-size' from='" + std::string(from) +
              "' to='12px' dur='2s'/>" +
              (animatedParent ? "<animate href='#parent' attributeName='font-size' from='12' "
                                "to='20' dur='2s'/>"
                              : ""),
          parser::SVGParser::Options(), Vector2i(96, 48));
      auto expected = instantiateSubtree(std::string("<text x='0' y='30' font-size='") +
                                             (animatedParent ? "22" : "18") + "'>H</text>",
                                         parser::SVGParser::Options(), Vector2i(96, 48));
      document.setTime(1);
      editor::tests::CompareBitmapToBitmap(
          RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
          "font-size-parent-context", editor::tests::PixelmatchIdentityParams());
      auto restored = instantiateSubtree(source, parser::SVGParser::Options(), Vector2i(96, 48));
      document.setTime(3);
      editor::tests::CompareBitmapToBitmap(
          RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(restored),
          "font-size-parent-context-restored", editor::tests::PixelmatchIdentityParams());
    }
  }
}
TEST(RendererAnimationStyleBase, FontStackPreservesKeywordsPrecedenceAndCascade) {
  struct Case {
    const char* name;
    const char* style;
    const char* animation;
    int expected;
  };
  for (const Case item : {
           Case{"negative-by", "",
                "<animate href='#label' attributeName='font-size' by='-4' dur='2s'/>", 6},
           Case{"negative-from-by", "",
                "<animate href='#label' attributeName='font-size' from='20' by='-4' dur='2s'/>",
                18},
           Case{"negative-relative-by", "",
                "<animate href='#label' attributeName='font-size' by=' -0.2em ' dur='2s'/>", 6},
           Case{"negative-additive", "",
                "<animate href='#label' attributeName='font-size' values='-4;0' additive='sum' "
                "dur='2s'/>",
                6},
           Case{"to-only-child-base", "",
                "<animate href='#label' attributeName='font-size' to='12px' dur='2s'/>"
                "<set href='#parent' attributeName='font-size' to='20px' dur='2s'/>",
                10},
           Case{"values-precedence", "",
                "<animate href='#label' attributeName='font-size' values='12px;20px' "
                "from='invalid' to='invalid' by='invalid' dur='2s'/>",
                16},
           Case{"inherit-additive", "",
                "<set href='#label' attributeName='font-size' to='inherit' dur='2s'/>"
                "<animate href='#label' attributeName='font-size' from='0' to='4' "
                "additive='sum' dur='2s'/>",
                22},
           Case{"initial-to-only", "",
                "<set href='#label' attributeName='font-size' to='initial' dur='2s'/>"
                "<animate href='#label' attributeName='font-size' to='20' dur='2s'/>",
                16},
           Case{"important", "style='font-size:18px !important'",
                "<animate href='#label' attributeName='font-size' from='12' to='20' dur='2s'/>",
                18},
           Case{"negative", "", "<set href='#label' attributeName='font-size' to='-4' dur='2s'/>",
                8},
       }) {
    SCOPED_TRACE(item.name);
    auto document = instantiateSubtree(
        std::string("<g id='parent' font-size='20'><text id='label' x='0' y='30' font-size='8' ") +
            item.style + ">H</text></g>" + item.animation,
        parser::SVGParser::Options(), Vector2i(96, 48));
    auto expected = instantiateSubtree(
        "<text x='0' y='30' font-size='" + std::to_string(item.expected) + "'>H</text>",
        parser::SVGParser::Options(), Vector2i(96, 48));
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected), item.name,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationStyleBase, SharedTextResolvesFontAnimationPerShadowParent) {
  auto document = instantiateSubtree(
      "<defs><text id='label' y='30' fill='red'>H<animate attributeName='font-size' from='100%' "
      "to='2em' dur='2s'/></text></defs><use href='#label' font-size='12'/>"
      "<use href='#label' x='48' font-size='20'/>",
      parser::SVGParser::Options(), Vector2i(96, 48));
  auto expected = instantiateSubtree(
      "<text y='30' fill='red' font-size='18'>H</text><text x='48' y='30' fill='red' "
      "font-size='30'>H</text>",
      parser::SVGParser::Options(), Vector2i(96, 48));
  document.setTime(1);
  const auto actual = RenderDocumentWithActiveBackend(document);
  const auto label = document.querySelector("#label")->entityHandle().entity();
  std::vector<double> sizes;
  for (const auto entity :
       document.registry()
           .view<components::ShadowEntityComponent, components::ComputedStyleComponent>()) {
    if (document.registry().get<components::ShadowEntityComponent>(entity).lightEntity == label) {
      sizes.push_back(document.registry()
                          .get<components::ComputedStyleComponent>(entity)
                          .properties->fontSize.get()
                          .value()
                          .value);
    }
  }
  std::sort(sizes.begin(), sizes.end());
  EXPECT_THAT(sizes, testing::ElementsAre(18, 30));
  editor::tests::CompareBitmapToBitmap(actual, RenderDocumentWithActiveBackend(expected),
                                       "shadow-parent-font-context",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationStyleBase, AnimatedParentKeepsRelativeSpanFontsAndBaselines) {
  auto document = instantiateSubtree(
      "<defs><text id='label' y='30' fill='red' text-decoration='underline'>"
      "<tspan font-size='50%' baseline-shift='0.25em'>Hi</tspan>"
      "<animate attributeName='font-size' from='100%' to='2em' dur='2s'/></text></defs>"
      "<use href='#label' font-size='12'/><use href='#label' x='48' font-size='20'/>",
      parser::SVGParser::Options(), Vector2i(96, 48));
  auto expected = instantiateSubtree(
      "<g fill='red' text-decoration='underline'><text y='30' font-size='18'>"
      "<tspan font-size='9' baseline-shift='0.25em'>Hi</tspan></text>"
      "<text x='48' y='30' font-size='30'><tspan font-size='15' baseline-shift='0.25em'>Hi</tspan>"
      "</text></g>",
      parser::SVGParser::Options(), Vector2i(96, 48));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "animated-parent-relative-span-font", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationStyleBase, ShadowBaselineKeywordsDoNotLeakBetweenInstances) {
  auto document = instantiateSubtree(
      "<style>.sub .shift {baseline-shift:sub;}</style><defs><text id='label' y='30' "
      "font-size='20'><tspan class='shift'>H</tspan></text></defs>"
      "<use href='#label' class='sub'/><use href='#label' x='40'/>"
      "<use href='#label' class='sub' x='80'/>",
      parser::SVGParser::Options(), Vector2i(120, 48));
  auto expected = instantiateSubtree(
      "<text y='30' font-size='20'><tspan baseline-shift='sub'>H</tspan></text>"
      "<text x='40' y='30' font-size='20'>H</text>"
      "<text x='80' y='30' font-size='20'><tspan baseline-shift='sub'>H</tspan></text>",
      parser::SVGParser::Options(), Vector2i(120, 48));
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "shadow-baseline-keyword-order", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationStyleBase, MissingSpanStyleKeepsTextRootBaselineFallback) {
  auto document = instantiateSubtree(
      "<text id='root' y='30' font-size='20' baseline-shift='super'>"
      "<tspan id='span'>H</tspan></text>",
      parser::SVGParser::Options(), Vector2i(48, 48));
  (void)RenderDocumentWithActiveBackend(document);
  auto root = document.querySelector("#root")->entityHandle();
  auto span = document.querySelector("#span")->entityHandle();
  document.registry().remove<components::ComputedStyleComponent>(span.entity());
  auto& text = root.get<components::ComputedTextComponent>();
  document.registry().ctx().get<TextEngine>().resolvePerSpanLayoutStyles(root, text);
  using Span = components::ComputedTextComponent::TextSpan;
  ASSERT_THAT(text.spans, testing::Contains(testing::Field(&Span::sourceEntity, span.entity())));
  const auto found = std::find_if(text.spans.begin(), text.spans.end(), [&](const Span& value) {
    return value.sourceEntity == span.entity();
  });
  EXPECT_THAT(found->baselineShift, testing::Eq(Lengthd(0)));
  EXPECT_THAT(found->baselineShiftKeyword, testing::Eq(Span::BaselineShiftKeyword::Length));
  EXPECT_THAT(found->ancestorBaselineShifts, testing::IsEmpty());
}

TEST(RendererAnimationStyleBase, FontOverrideKeepsTypedPriority) {
  auto document = instantiateSubtree(
      "<text id='label' y='30'>H<animate attributeName='font-size' from='12' to='24' "
      "dur='2s'/></text>",
      parser::SVGParser::Options(), Vector2i(64, 48));
  document.querySelector("#label")
      ->entityHandle()
      .get_or_emplace<components::StyleComponent>()
      .properties.fontSize.set(Lengthd(20), css::Specificity::Override());
  auto expected = instantiateSubtree("<text y='30' font-size='20'>H</text>",
                                     parser::SVGParser::Options(), Vector2i(64, 48));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "font-typed-priority", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationStyleBase, ShadowFontDrawsIgnorePrewarmedLightGeometry) {
  auto document = instantiateSubtree(
      "<text id='label' y='30' fill='red'>H<animate attributeName='font-size' from='100%' "
      "to='2em' dur='2s'/></text><use href='#label' y='50' font-size='12'/>"
      "<use href='#label' x='48' y='50' font-size='20'/>"
      "<use href='#label' x='96' y='50' font-size='12'/>",
      parser::SVGParser::Options(), Vector2i(144, 96));
  auto expected = instantiateSubtree(
      "<g fill='red'><text y='30' font-size='18'>H</text><text y='80' font-size='18'>H</text>"
      "<text x='48' y='80' font-size='30'>H</text><text x='96' y='80' font-size='18'>H</text></g>",
      parser::SVGParser::Options(), Vector2i(144, 96));
  document.setTime(1);
  (void)RenderDocumentWithActiveBackend(document);
  auto label = document.querySelector("#label")->entityHandle();
  document.registry().ctx().get<TextEngine>().ensureComputedTextGeometryComponent(label);
  for (int redraw = 0; redraw < 2; ++redraw) {
    SCOPED_TRACE(redraw);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "shadow-font-cache-order", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationStyleBase, RepeatedFontRecipesShareOneWorkAllowance) {
  std::string values = "16";
  for (int i = 1; i < 512; ++i) {
    values += ";16";
  }
  std::string source =
      "<defs><g id='font' font-size='8'><animate attributeName='font-size' "
      "values='" +
      values + "' dur='2s'/></g></defs>";
  for (int i = 0; i < 100; ++i) {
    source += "<use href='#font'/>";
  }
  auto document = instantiateSubtree(source, parser::SVGParser::Options(), Vector2i(16, 16));
  document.setTime(1);
  (void)RenderDocumentWithActiveBackend(document);
  EXPECT_EQ(document.registry()
                .ctx()
                .get<components::AnimationResourceBudget>()
                .remainingContextualWork(),
            0);
  const auto target = document.querySelector("#font")->entityHandle().entity();
  std::vector<double> sizes;
  for (const auto entity :
       document.registry()
           .view<components::ShadowEntityComponent, components::ComputedStyleComponent>()) {
    if (document.registry().get<components::ShadowEntityComponent>(entity).lightEntity == target) {
      sizes.push_back(document.registry()
                          .get<components::ComputedStyleComponent>(entity)
                          .properties->fontSize.get()
                          .value()
                          .value);
    }
  }
  EXPECT_THAT(sizes, testing::SizeIs(100));
  EXPECT_THAT(sizes, testing::Each(testing::AnyOf(8, 16)));
  EXPECT_THAT(sizes, testing::Contains(8));
  EXPECT_THAT(sizes, testing::Contains(16));
}

#endif

TEST(RendererAnimationStyleBase, ToOnlyPaintKeepsStylesheetReferenceAndFallback) {
  for (const std::string_view paint : {"url('#g')", "url('#quoted\"g')", "url('#missing') red"}) {
    SCOPED_TRACE(paint);
    const std::string base =
        "<defs><linearGradient id='g'><stop stop-color='red'/><stop offset='1' "
        "stop-color='blue'/></linearGradient><linearGradient id='quoted\"g' href='#g'/></defs>"
        "<style>#r { fill:" +
        std::string(paint) + "; }</style><rect id='r' width='16' height='16'/>";
    auto document =
        instantiateSubtree(base + "<animate href='#r' attributeName='fill' to='green' dur='2s'/>",
                           parser::SVGParser::Options(), Vector2i(16, 16));
    auto expected = instantiateSubtree(base, parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(0.5);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "stylesheet-paint-base", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationStyleBase, ToOnlyDashListUsesStylesheetValue) {
  auto document = instantiateSubtree(
      "<style>#p {stroke-dasharray:2 2;}</style><path id='p' d='M0 8H16' stroke='black'>"
      "<animate attributeName='stroke-dasharray' to='6 2' dur='2s'/></path>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  auto expected = instantiateSubtree("<path d='M0 8H16' stroke='black' stroke-dasharray='4 2'/>",
                                     parser::SVGParser::Options(), Vector2i(16, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "stylesheet-dash-base", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationStyleBase, NoneDashListRemainsDiscrete) {
  auto document = instantiateSubtree(
      "<path d='M0 8H16' stroke='black'>"
      "<animate attributeName='stroke-dasharray' to='4' dur='2s'/></path>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  auto expected = instantiateSubtree("<path d='M0 8H16' stroke='black'/>",
                                     parser::SVGParser::Options(), Vector2i(16, 16));
  document.setTime(0.5);
  editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                       RenderDocumentWithActiveBackend(expected), "none-dash-base",
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
