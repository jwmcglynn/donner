#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <tuple>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/paint/StopComponent.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleComponent.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "donner/svg/tests/ParserTestUtils.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg {
namespace {

TEST(RendererAnimationStyleBase, PaintContextUsesSampledInstanceStyles) {
  struct Case {
    const char* name;
    const char* animated;
    const char* expected;
  };
  for (const Case item : {
           Case{"same-frame-current-color",
                "<rect width='16' height='16' color='black'>"
                "<animate attributeName='fill' from='currentColor' to='blue' dur='2s'/>"
                "<set attributeName='color' to='red'/></rect>",
                "<rect width='16' height='16' fill='#800080'/>"},
           Case{"stroke-sampled-current-color",
                "<path d='M0 8H16' stroke-width='16' color='black'>"
                "<animate attributeName='stroke' from='currentColor' to='blue' dur='2s'/>"
                "<set attributeName='color' to='red'/></path>",
                "<rect width='16' height='16' fill='#800080'/>"},
           Case{"sampled-parent-declared-after-child",
                "<g color='black'><rect width='16' height='16'>"
                "<animate attributeName='fill' from='currentColor' to='blue' dur='2s'/>"
                "</rect><set attributeName='color' to='red'/></g>",
                "<rect width='16' height='16' fill='#800080'/>"},
           Case{"important-current-color",
                "<rect width='16' height='16' style='color:lime!important'>"
                "<animate attributeName='fill' from='currentColor' to='blue' dur='2s'/>"
                "<set attributeName='color' to='red'/></rect>",
                "<rect width='16' height='16' fill='#008080'/>"},
           Case{"important-fill",
                "<rect width='16' height='16' style='fill:yellow!important'>"
                "<animate attributeName='fill' from='currentColor' to='blue' dur='2s'/>"
                "<set attributeName='color' to='red'/></rect>",
                "<rect width='16' height='16' fill='yellow'/>"},
           Case{"shadow-instance-colors",
                "<defs><rect id='shape' width='8' height='16'>"
                "<animate attributeName='fill' from='currentColor' to='black' dur='2s'/>"
                "</rect></defs><use href='#shape' color='red'/>"
                "<use href='#shape' x='8' color='blue'/>"
                "<use href='#shape' x='16' color='red'/>",
                "<rect width='8' height='16' fill='#800000'/>"
                "<rect x='8' width='8' height='16' fill='#000080'/>"
                "<rect x='16' width='8' height='16' fill='#800000'/>"},
           Case{"color-current-color-parent",
                "<g color='red'><rect width='16' height='16' color='lime' fill='currentColor'>"
                "<animate attributeName='color' from='currentColor' to='blue' dur='2s'/>"
                "</rect></g>",
                "<rect width='16' height='16' fill='#800080'/>"},
           Case{"lower-inherited-paint",
                "<g fill='red'><rect width='16' height='16' fill='lime'>"
                "<set attributeName='fill' to='inherit'/>"
                "<animate attributeName='fill' to='black' dur='2s'/></rect></g>",
                "<rect width='16' height='16' fill='#800000'/>"},
           Case{"lower-unset-paint",
                "<g fill='red'><rect width='16' height='16' fill='lime'>"
                "<set attributeName='fill' to='unset'/>"
                "<animate attributeName='fill' to='black' dur='2s'/></rect></g>",
                "<rect width='16' height='16' fill='#800000'/>"},
           Case{"explicit-child-paint",
                "<g><set attributeName='fill' to='red'/>"
                "<set attributeName='stroke' to='red'/><set attributeName='color' to='red'/>"
                "<rect width='8' height='8' fill='blue' stroke='none'/>"
                "<path d='M8 4H16' stroke='blue' stroke-width='8'/>"
                "<rect y='8' width='16' height='8' color='blue' fill='currentColor' "
                "stroke='none'/></g>",
                "<rect width='16' height='16' fill='blue'/>"},
           Case{"sampled-interpolation-mode",
                "<rect width='16' height='16' color-interpolation='sRGB'>"
                "<animate attributeName='fill' from='black' to='white' dur='2s'/>"
                "<set attributeName='color-interpolation' to='linearRGB'/></rect>",
                "<rect width='16' height='16' fill='#bcbcbc'/>"},
           Case{"contextual-url-fallback",
                "<rect width='16' height='16' color='black' fill='url(#missing) currentColor'>"
                "<animate attributeName='fill' to='none' calcMode='discrete' dur='4s'/>"
                "<set attributeName='color' to='red'/></rect>",
                "<rect width='16' height='16' fill='red'/>"},
       }) {
    SCOPED_TRACE(item.name);
    auto document =
        instantiateSubtree(item.animated, parser::SVGParser::Options(), Vector2i(24, 16));
    auto expected =
        instantiateSubtree(item.expected, parser::SVGParser::Options(), Vector2i(24, 16));
    const auto source = document.source();
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected), item.name,
                                         editor::tests::PixelmatchIdentityParams());
    document.setTime(0.5);
    std::ignore = RenderDocumentWithActiveBackend(document);
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        std::string(item.name) + "-resampled", editor::tests::PixelmatchIdentityParams());
    EXPECT_EQ(document.source(), source);
  }
}

TEST(RendererAnimationStyleBase, InvalidPaintEndpointsPreserveLowerLayers) {
  struct Case {
    const char* name;
    const char* operands;
    double time;
    const char* expected;
  };
  for (const Case item : {
           Case{"invalid-later-endpoint", "from='blue' to='not-a-color'", 0.5, "red"},
           Case{"invalid-later-keyframe", "values='blue;lime;not-a-color'", 0.5, "red"},
           Case{"ignored-operands-with-values",
                "values='red;blue' from='not-a-color' by='not-a-color'", 1, "#800080"},
           Case{"ignored-by-with-to", "from='red' to='blue' by='not-a-color'", 1, "#800080"},
       }) {
    SCOPED_TRACE(item.name);
    auto document = instantiateSubtree(
        "<rect width='16' height='16' fill='black'>"
        "<set attributeName='fill' to='red'/>"
        "<animate attributeName='fill' " +
            std::string(item.operands) + " dur='2s'/></rect>",
        parser::SVGParser::Options(), Vector2i(16, 16));
    auto expected = instantiateSubtree(
        "<rect width='16' height='16' fill='" + std::string(item.expected) + "'/>",
        parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(item.time);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected), item.name,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationStyleBase, ResourceColorsUseSampledCurrentColor) {
  struct Case {
    const char* name;
    const char* animated;
    const char* expected;
  };
  for (const Case item : {
           Case{"animated-stop-current-color",
                "<defs><linearGradient id='g'><stop color='black'>"
                "<animate attributeName='stop-color' from='currentColor' to='blue' dur='2s'/>"
                "<set attributeName='color' to='red'/></stop></linearGradient></defs>"
                "<rect width='16' height='16' fill='url(#g)'/>",
                "<rect width='16' height='16' fill='#800080'/>"},
           Case{"important-stop-color",
                "<defs><linearGradient id='g'><stop style='stop-color:yellow!important'>"
                "<animate attributeName='stop-color' from='currentColor' to='blue' dur='2s'/>"
                "<set attributeName='color' to='red'/></stop></linearGradient></defs>"
                "<rect width='16' height='16' fill='url(#g)'/>",
                "<rect width='16' height='16' fill='yellow'/>"},
           Case{"animated-flood-current-color",
                "<defs><filter id='f'><feFlood color='black'>"
                "<animate attributeName='flood-color' from='currentColor' to='blue' dur='2s'/>"
                "<set attributeName='color' to='red'/></feFlood></filter></defs>"
                "<rect width='16' height='16' filter='url(#f)'/>",
                "<rect width='16' height='16' fill='#800080'/>"},
       }) {
    SCOPED_TRACE(item.name);
    auto document =
        instantiateSubtree(item.animated, parser::SVGParser::Options(), Vector2i(16, 16));
    auto expected =
        instantiateSubtree(item.expected, parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected), item.name,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationStyleBase, AnimatedLightingColorMatchesStaticLighting) {
  for (const std::string primitive : {"feDiffuseLighting", "feSpecularLighting"}) {
    SCOPED_TRACE(primitive);
    const std::string prefix =
        "<defs><filter id='f' x='0' y='0' width='1' height='1'><" + primitive;
    const std::string suffix = "<feDistantLight azimuth='0' elevation='90'/></" + primitive +
                               "></filter></defs><rect width='16' height='16' filter='url(#f)'/>";
    auto document = instantiateSubtree(
        prefix +
            " lighting-color='black'>"
            "<animate attributeName='lighting-color' from='red' to='blue' dur='2s'/>" +
            suffix,
        parser::SVGParser::Options(), Vector2i(16, 16));
    auto expected = instantiateSubtree(prefix + " lighting-color='#800080'>" + suffix,
                                       parser::SVGParser::Options(), Vector2i(16, 16));
    const auto expectedBitmap = RenderDocumentWithActiveBackend(expected);
    EXPECT_THAT(test::PixelAt(expectedBitmap, 8, 8),
                test::Rgba(testing::Gt(0), 0, testing::Gt(0), testing::Gt(0)));
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document), expectedBitmap,
                                         "animated-" + primitive + "-color",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

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

TEST(RendererAnimationStyleBase, ScalarCompositionUsesDimensionlessOperands) {
  struct Case {
    const char* animation;
    const char* expected;
    double time = 1;
  };
  for (const Case item : {
           Case{"values='100%;0.5;0' calcMode='paced'", "0.5"},
           Case{"by='-50%'", "0"},
           Case{"from='0' to='50%' additive='sum'", "0.5"},
           Case{"from='0' to='25%' accumulate='sum' repeatCount='3'", "0.375", 3},
           Case{"values='100%;0' from='1px' to='invalid' by='invalid'", "0.5"},
           Case{"values='INITIAL;0' calcMode='discrete'", "0", 1.5},
           Case{"values='InHeRiT;0' calcMode='discrete'", "0", 1.5},
           Case{"values='UnSeT;0' calcMode='discrete'", "0", 1.5},
           Case{"values='100%;0;1px'", "0.25"},
           Case{"from='1px' by='3'", "0.25"},
       }) {
    SCOPED_TRACE(item.animation);
    auto document = instantiateSubtree(std::string("<rect width='16' height='16' opacity='0.25'>") +
                                           "<animate attributeName='opacity' dur='2s' " +
                                           item.animation + "/></rect>",
                                       parser::SVGParser::Options(), Vector2i(16, 16));
    auto expected = instantiateSubtree(
        std::string("<rect width='16' height='16' opacity='") + item.expected + "'/>",
        parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(item.time);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "scalar-composition", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationStyleBase, LowerInitialOpacityIsResolvedBeforeToOnlyInterpolation) {
  auto document = instantiateSubtree(
      "<rect width='16' height='16' opacity='0.2'><set attributeName='opacity' to='INITIAL' "
      "dur='2s'/>"
      "<animate attributeName='opacity' to='0' dur='2s'/></rect>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  auto expected = instantiateSubtree("<rect width='16' height='16' opacity='0.5'/>",
                                     parser::SVGParser::Options(), Vector2i(16, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "scalar-lower-initial", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationStyleBase, ResourceOpacityPreservesStylesAndTypedPriority) {
  for (const bool flood : {false, true}) {
    for (const bool important : {false, true}) {
      for (const bool typed : {false, true}) {
        SCOPED_TRACE(flood);
        SCOPED_TRACE(important);
        SCOPED_TRACE(typed);
        const std::string attribute = flood ? "flood-opacity" : "stop-opacity";
        const std::string source =
            flood ? "<defs><filter id='f'><feFlood id='r' flood-color='red' "
                    "flood-opacity='0.8'/></filter></defs><rect width='16' height='16' "
                    "filter='url(#f)'/>"
                  : "<defs><linearGradient id='g'><stop id='r' stop-color='red' "
                    "stop-opacity='0.8'/></linearGradient></defs><rect width='16' height='16' "
                    "fill='url(#g)'/>";
        auto document = instantiateSubtree(
            source + "<style>#r {" + attribute + ":0.6" + (important ? " !important" : "") +
                ";}</style><animate href='#r' attributeName='" + attribute + "' to='0' dur='2s'/>",
            parser::SVGParser::Options(), Vector2i(16, 16));
        if (typed) {
          auto target = document.querySelector("#r")->entityHandle();
          auto& opacity = flood ? target.get<components::FEFloodComponent>().floodOpacity
                                : target.get<components::StopComponent>().properties.stopOpacity;
          opacity.set(0.9, css::Specificity::Override());
        }
        std::string expectedSource = source;
        const auto offset = expectedSource.find(attribute + "='0.8'");
        ASSERT_NE(offset, std::string::npos);
        expectedSource.replace(offset, attribute.size() + 6,
                               attribute + "='" +
                                   (typed       ? "0.9"
                                    : important ? "0.6"
                                                : "0.3") +
                                   "'");
        auto expected =
            instantiateSubtree(expectedSource, parser::SVGParser::Options(), Vector2i(16, 16));
        document.setTime(1);
        editor::tests::CompareBitmapToBitmap(
            RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
            "resource-opacity-cascade", editor::tests::PixelmatchIdentityParams());
        document.setTime(3);
        EXPECT_THAT(document.querySelector("#r")->getAttribute(attribute.c_str()),
                    testing::Optional(RcString("0.8")));
      }
    }
  }
}

TEST(RendererAnimationStyleBase, InheritedScalarLayersUseTheSampledParent) {
  for (const std::string attribute : {"opacity", "fill-opacity"}) {
    for (const bool important : {false, true}) {
      for (const std::string keyword : {"inherit", "unset"}) {
        SCOPED_TRACE(attribute);
        SCOPED_TRACE(important);
        SCOPED_TRACE(keyword);
        const std::string parentValue = important ? "0.6" : "0.4";
        const std::string childValue = attribute == "opacity" && keyword == "unset" ? "0.5"
                                       : important                                  ? "0.3"
                                                                                    : "0.2";
        auto document = instantiateSubtree(
            "<g id='parent' style='" + attribute + ":" + (important ? "0.6!important" : "0.8") +
                "'><rect id='r' width='16' height='16' " + attribute +
                "='0.9'/></g>"
                "<set href='#r' attributeName='" +
                attribute + "' to='" + keyword +
                "' dur='2s'/>"
                "<animate href='#r' attributeName='" +
                attribute +
                "' to='0' dur='2s'/>"
                "<animate href='#parent' attributeName='" +
                attribute + "' from='0.2' to='0.6' dur='2s'/>",
            parser::SVGParser::Options(), Vector2i(16, 16));
        auto expected = instantiateSubtree("<g " + attribute + "='" + parentValue +
                                               "'><rect width='16' height='16' " + attribute +
                                               "='" + childValue + "'/></g>",
                                           parser::SVGParser::Options(), Vector2i(16, 16));
        document.setTime(1);
        editor::tests::CompareBitmapToBitmap(
            RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
            "scalar-parent-keyword", editor::tests::PixelmatchIdentityParams());
      }
    }
  }
}

TEST(RendererAnimationStyleBase, InheritedScalarLayersUseEachShadowParent) {
  auto document = instantiateSubtree(
      "<defs><rect id='shape' width='16' height='16' fill='red' fill-opacity='0.9'>"
      "<set attributeName='fill-opacity' to='inherit' dur='2s'/>"
      "<animate attributeName='fill-opacity' to='0' dur='2s'/></rect></defs>"
      "<use href='#shape' fill-opacity='0.4'/><use href='#shape' x='16' fill-opacity='0.8'/>",
      parser::SVGParser::Options(), Vector2i(32, 16));
  auto expected = instantiateSubtree(
      "<rect width='16' height='16' fill='red' fill-opacity='0.2'/>"
      "<rect x='16' width='16' height='16' fill='red' fill-opacity='0.4'/>",
      parser::SVGParser::Options(), Vector2i(32, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "scalar-shadow-parent", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationStyleBase, ResourceOpacityInheritsTheSampledParentValue) {
  for (const bool flood : {false, true}) {
    SCOPED_TRACE(flood);
    const std::string attribute = flood ? "flood-opacity" : "stop-opacity";
    const std::string source =
        flood ? "<defs><filter id='parent' style='flood-opacity:0.8'><feFlood id='r' "
                "flood-color='red'/></filter></defs><rect width='16' height='16' "
                "filter='url(#parent)'/>"
              : "<defs><linearGradient id='parent' style='stop-opacity:0.8'><stop id='r' "
                "stop-color='red'/></linearGradient></defs><rect width='16' height='16' "
                "fill='url(#parent)'/>";
    auto document = instantiateSubtree(source + "<set href='#r' attributeName='" + attribute +
                                           "' to='inherit' dur='2s'/>"
                                           "<animate href='#r' attributeName='" +
                                           attribute +
                                           "' to='0' dur='2s'/>"
                                           "<animate href='#parent' attributeName='" +
                                           attribute + "' from='0.2' to='0.6' dur='2s'/>",
                                       parser::SVGParser::Options(), Vector2i(16, 16));
    std::string reference = source;
    reference.insert(reference.find("id='r'"), attribute + "='0.2' ");
    auto expected = instantiateSubtree(reference, parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "resource-opacity-parent", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationStyleBase, ScalarStackDoesNotClampIntermediateLayers) {
  auto document = instantiateSubtree(
      "<rect width='16' height='16' opacity='0.2'><set attributeName='opacity' to='2' dur='2s'/>"
      "<animate attributeName='opacity' to='0' dur='2s'/></rect>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  auto expected = instantiateSubtree("<rect width='16' height='16'/>", parser::SVGParser::Options(),
                                     Vector2i(16, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "scalar-unclamped-stack", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationStyleBase, InheritedScalarStacksRestoreAfterSeekingAndSourceRemoval) {
  auto document = instantiateSubtree(
      "<g fill-opacity='0.6'><rect width='16' height='16' fill-opacity='0.8'>"
      "<set id='lower' attributeName='fill-opacity' to='inherit' dur='2s'/>"
      "<animate id='upper' attributeName='fill-opacity' to='0' dur='2s'/></rect></g>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  for (const double time : {1.0, 3.0, 1.0}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    auto expected = instantiateSubtree(std::string("<rect width='16' height='16' fill-opacity='") +
                                           (time == 1 ? "0.3" : "0.8") + "'/>",
                                       parser::SVGParser::Options(), Vector2i(16, 16));
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "scalar-inherit-seeking", editor::tests::PixelmatchIdentityParams());
  }
  for (const std::string id : {"#upper", "#lower"}) {
    SCOPED_TRACE(id);
    const auto animation = document.querySelector(id);
    ASSERT_THAT(animation, testing::Optional(testing::_));
    ASSERT_THAT(document.removeElement(*animation).applied, testing::IsTrue());
    auto expected = instantiateSubtree(std::string("<rect width='16' height='16' fill-opacity='") +
                                           (id == "#upper" ? "0.6" : "0.8") + "'/>",
                                       parser::SVGParser::Options(), Vector2i(16, 16));
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "scalar-inherit-source-removal", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationStyleBase, ScalarInheritanceUsesTheParentsFinalClampedValue) {
  auto document = instantiateSubtree(
      "<g id='parent' opacity='0.2'><rect id='r' width='16' height='16'/></g>"
      "<set href='#r' attributeName='opacity' to='inherit' dur='2s'/>"
      "<animate href='#r' attributeName='opacity' to='0' dur='2s'/>"
      "<set href='#parent' attributeName='opacity' to='2' dur='2s'/>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  auto expected = instantiateSubtree("<rect width='16' height='16' opacity='0.5'/>",
                                     parser::SVGParser::Options(), Vector2i(16, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "scalar-parent-final-clamp", editor::tests::PixelmatchIdentityParams());
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
