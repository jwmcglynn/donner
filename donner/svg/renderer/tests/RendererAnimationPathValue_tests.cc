#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/components/text/TextPathComponent.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

TEST(RendererAnimationPathValue, CompositionUsesPathParametersAndPreservesIncompatibleBase) {
  struct Case {
    const char* name;
    const char* operands;
    double sampleTime;
    double endTime;
    int sampledSize;
    int frozenSize;
    bool addsToBase;
  };
  for (const Case item : {
           Case{"from-by", "from='M0 0L2 0L2 2L0 2Z' by='M0 0L4 0L4 4L0 4Z'", 1, 3, 4, 6, false},
           Case{"by-only", "by='M0 0L4 0L4 4L0 4Z'", 1, 3, 6, 8, true},
           Case{"additive", "from='M0 0L2 0L2 2L0 2Z' to='M0 0L4 0L4 4L0 4Z' additive='sum'", 1, 3,
                7, 8, true},
           Case{"accumulated",
                "from='M0 0L2 0L2 2L0 2Z' to='M0 0L4 0L4 4L0 4Z' "
                "accumulate='sum' repeatCount='2'",
                3, 5, 7, 8, false},
           Case{"accumulated-by", "by='M0 0L4 0L4 4L0 4Z' accumulate='sum' repeatCount='2'", 3, 5,
                10, 12, true},
           Case{"incompatible-additive", "from='M0 0Q2 0 2 2Z' to='M0 0Q4 0 4 4Z' additive='sum'",
                1, 3, 4, 4, true},
           Case{"incompatible-from-by", "from='M0 0Q2 0 2 2Z' by='M0 0L4 0L4 4L0 4Z'", 1, 3, 4, 4,
                true},
           Case{"incompatible-by-only", "by='M0 0Q4 0 4 4Z'", 1, 3, 4, 4, true},
       }) {
    SCOPED_TRACE(item.name);
    auto document = instantiateSubtree(
        "<path fill='red' d='M2 2L6 2L6 6L2 6Z'><animate attributeName='d' dur='2s' "
        "fill='freeze' " +
            std::string(item.operands) + "/></path>",
        parser::SVGParser::Options(), Vector2i(32, 32));
    const std::string source(document.source());
    for (const double time : {item.sampleTime, item.endTime, item.sampleTime}) {
      SCOPED_TRACE(time);
      const int size = time == item.endTime ? item.frozenSize : item.sampledSize;
      const std::string origin = item.addsToBase ? "2" : "0";
      auto expected =
          instantiateSubtree("<rect fill='red' x='" + origin + "' y='" + origin + "' width='" +
                                 std::to_string(size) + "' height='" + std::to_string(size) + "'/>",
                             parser::SVGParser::Options(), Vector2i(32, 32));
      document.setTime(time);
      editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                           RenderDocumentWithActiveBackend(expected), item.name,
                                           editor::tests::PixelmatchIdentityParams());
      EXPECT_EQ(document.source(), source);
    }
  }
}

TEST(RendererAnimationPathValue, InterpolationRetainsAuthoredCommandsAndArcParameters) {
  struct Case {
    const char* name;
    const char* from;
    const char* to;
    double time;
    const char* expected;
  };
  for (const Case item : {
           Case{"different-command-types", "M0 0H4V4H0Z", "M0 0L8 0L8 8L0 8Z", 0.5,
                "<rect width='4' height='4'/>"},
           Case{"different-command-types-late", "M0 0H4V4H0Z", "M0 0L8 0L8 8L0 8Z", 1.5,
                "<rect width='8' height='8'/>"},
           Case{"implicit-line-commands", "M0 0 4 0 4 4 0 4Z", "M0 0L8 0L8 8L0 8Z", 1,
                "<rect width='6' height='6'/>"},
           Case{"closepath-case", "M0 0L4 0L4 4L0 4Z", "M0 0L8 0L8 8L0 8z", 1,
                "<rect width='6' height='6'/>"},
           Case{"arc-radii", "M8 16A8 8 0 0 1 24 16", "M8 16A12 8 0 0 1 24 16", 1,
                "<path d='M8 16A10 8 0 0 1 24 16'/>"},
           Case{"arc-sweep-flag", "M8 16A10 8 0 0 0 24 16", "M8 16A10 8 0 0 1 24 16", 0.5,
                "<path d='M8 16A10 8 0 0 1 24 16'/>"},
           Case{"arc-sweep-start", "M8 16A10 8 0 0 0 24 16", "M8 16A10 8 0 0 1 24 16", 0,
                "<path d='M8 16A10 8 0 0 0 24 16'/>"},
           Case{"arc-sweep-freeze", "M8 16A10 8 0 0 0 24 16", "M8 16A10 8 0 0 1 24 16", 3,
                "<path d='M8 16A10 8 0 0 1 24 16'/>"},
       }) {
    SCOPED_TRACE(item.name);
    auto document =
        instantiateSubtree("<path><animate attributeName='d' from='" + std::string(item.from) +
                               "' to='" + item.to + "' dur='2s' fill='freeze'/></path>",
                           parser::SVGParser::Options(), Vector2i(32, 32));
    auto expected =
        instantiateSubtree(item.expected, parser::SVGParser::Options(), Vector2i(32, 32));
    document.setTime(item.time);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected), item.name,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationPathValue, EmptyPathSuppressesAndRestoresGeometry) {
  auto document = instantiateSubtree(
      "<path d='M2 2L6 2L6 6L2 6Z'><set attributeName='d' to='' dur='2s'/></path>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  const std::string source(document.source());
  for (const double time : {1.0, 3.0, 1.0}) {
    auto expected = instantiateSubtree(
        time < 2 ? "<rect width='0' height='0'/>" : "<rect x='2' y='2' width='4' height='4'/>",
        parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(time);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "empty-animated-path", editor::tests::PixelmatchIdentityParams());
    EXPECT_EQ(document.source(), source);
  }
}

TEST(RendererAnimationPathValue, InterpolationPreservesCoordinatePrecision) {
  auto document = instantiateSubtree(
      "<g transform='translate(-1000000 0)'><path>"
      "<animate attributeName='d' from='M1000001 2L1000005 2L1000005 6L1000001 6Z' "
      "to='M1000003 2L1000007 2L1000007 6L1000003 6Z' dur='2s'/></path></g>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  auto expected = instantiateSubtree("<rect x='2' y='2' width='4' height='4'/>",
                                     parser::SVGParser::Options(), Vector2i(16, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "path-coordinate-precision", editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationPathValue, IncompatibleCommandsUseDiscreteKeyframeTiming) {
  struct Case {
    const char* name;
    const char* timing;
    const char* values;
    double time;
    int size;
  };
  for (const Case item : {
           Case{"explicit-key-times", "keyTimes='0;1'", "M0 0H4V4H0Z;M0 0L8 0L8 8L0 8Z", 1.5, 4},
           Case{"spline-does-not-ease-discrete-values", "calcMode='spline' keySplines='0 0 0 1'",
                "M0 0H4V4H0Z;M0 0L8 0L8 8L0 8Z", 0.5, 4},
           Case{"whole-list-compatibility", "", "M0 0L4 0L4 4L0 4Z;M0 0L8 0L8 8L0 8Z;M0 0H12V12H0Z",
                0.5, 4},
       }) {
    SCOPED_TRACE(item.name);
    auto document =
        instantiateSubtree("<path><animate attributeName='d' values='" + std::string(item.values) +
                               "' dur='2s' " + item.timing + "/></path>",
                           parser::SVGParser::Options(), Vector2i(16, 16));
    auto expected = instantiateSubtree("<rect width='" + std::to_string(item.size) + "' height='" +
                                           std::to_string(item.size) + "'/>",
                                       parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(item.time);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected), item.name,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationPathValue, InlineTextPathRetainsInterpolation) {
  auto document = instantiateSubtree(
      "<text font-size='12'><textPath id='t' path='M0 12L32 12'>H"
      "<animate attributeName='path' from='M0 12L32 12' to='M0 28L32 28' dur='4s'/>"
      "</textPath></text>",
      parser::SVGParser::Options(), Vector2i(32, 32));
  document.setTime(1);
  const auto actual = RenderDocumentWithActiveBackend(document);
  const auto& path =
      document.querySelector("#t")->entityHandle().get<components::TextPathComponent>().inlinePath;
  ASSERT_THAT(path, testing::Ne(std::nullopt));
  EXPECT_THAT(path->points(), testing::ElementsAre(Vector2d(0, 16), Vector2d(32, 16)));
#ifdef DONNER_TEXT_ENABLED
  auto expected =
      instantiateSubtree("<text font-size='12'><textPath path='M0 16L32 16'>H</textPath></text>",
                         parser::SVGParser::Options(), Vector2i(32, 32));
  EXPECT_THAT(RendererTestUtils::renderToAsciiImage(expected).generated,
              testing::Contains(testing::Not(testing::AnyOf(testing::Eq('.'), testing::Eq('\n')))));
  editor::tests::CompareBitmapToBitmap(actual, RenderDocumentWithActiveBackend(expected),
                                       "inline-text-path-interpolation",
                                       editor::tests::PixelmatchIdentityParams());
#endif
}

TEST(RendererAnimationPathValue, IncompatibleUpperLayerPreservesTheLowerAnimation) {
  auto document = instantiateSubtree(
      "<path d='M0 0L4 0L4 4L0 4Z'>"
      "<set attributeName='d' to='M0 0L8 0L8 8L0 8Z' dur='10s'/>"
      "<animate attributeName='d' from='M0 0Q2 0 2 2Z' to='M0 0Q4 0 4 4Z' "
      "additive='sum' dur='2s' fill='freeze'/></path>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  auto expected = instantiateSubtree("<rect width='8' height='8'/>", parser::SVGParser::Options(),
                                     Vector2i(16, 16));
  for (const double time : {1.0, 3.0, 1.0}) {
    document.setTime(time);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "incompatible-path-lower-stack", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationPathValue, ToOnlyUsesTheCurrentLowerLayerAndRestores) {
  auto document = instantiateSubtree(
      "<path d='M0 0L4 0L4 4L0 4Z'>"
      "<animate attributeName='d' from='M0 0L4 0L4 4L0 4Z' to='M0 0L8 0L8 8L0 8Z' dur='4s'/>"
      "<animate attributeName='d' to='M0 0L12 0L12 12L0 12Z' dur='2s' additive='sum'/></path>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  const std::string source(document.source());
  for (const double time : {1.0, 3.0, 1.0}) {
    auto expected = instantiateSubtree(
        time == 1 ? "<rect width='8.5' height='8.5'/>" : "<rect width='7' height='7'/>",
        parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(time);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "to-only-path-lower-stack", editor::tests::PixelmatchIdentityParams());
    EXPECT_EQ(document.source(), source);
  }
}

TEST(RendererAnimationPathValue, PartialPathAndPacedFallbackRemainDefined) {
  for (const std::string timing : {"", "calcMode='paced' keyTimes='0;0.1'"}) {
    SCOPED_TRACE(timing);
    auto document = instantiateSubtree(
        "<path><animate attributeName='d' from='M0 0L4 0L4 4L0 4Zq5' "
        "to='M0 0L8 0L8 8L0 8Z' dur='2s' " +
            timing + "/></path>",
        parser::SVGParser::Options(), Vector2i(16, 16));
    auto expected = instantiateSubtree("<rect width='5' height='5'/>", parser::SVGParser::Options(),
                                       Vector2i(16, 16));
    document.setTime(0.5);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "path-prefix-and-linear-fallback", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationPathValue, ToOnlyUsesTypedAndStylesheetPathBases) {
  for (const std::string base :
       {"attribute", "stylesheet", "important", "typed-d", "typed-spline"}) {
    SCOPED_TRACE(base);
    std::string source;
    if (base == "stylesheet" || base == "important") {
      source = "<style>#p {d:'M0 0L8 0L8 8L0 8Z'";
      source += base == "important" ? " !important;}</style>" : ";}</style>";
    }
    source +=
        "<path id='p' d='M0 0L4 0L4 4L0 4Z'>"
        "<animate attributeName='d' to='M0 0L12 0L12 12L0 12Z' dur='2s'/></path>";
    auto document = instantiateSubtree(source, parser::SVGParser::Options(), Vector2i(16, 16));
    auto path = document.querySelector("#p")->cast<SVGPathElement>();
    if (base == "typed-d") {
      path.setD("M0 0L8 0L8 8L0 8Z");
    } else if (base == "typed-spline") {
      PathBuilder builder;
      builder.moveTo({0, 0}).lineTo({8, 0}).lineTo({8, 8}).lineTo({0, 8}).closePath();
      path.setSpline(builder.build());
    }
    const std::string sourceBefore(document.source());
    for (const double time : {1.0, 3.0, 1.0}) {
      SCOPED_TRACE(time);
      const int size = base == "important"   ? 8
                       : base == "attribute" ? (time == 1 ? 8 : 4)
                                             : (time == 1 ? 10 : 8);
      auto expected = instantiateSubtree(
          "<rect width='" + std::to_string(size) + "' height='" + std::to_string(size) + "'/>",
          parser::SVGParser::Options(), Vector2i(16, 16));
      document.setTime(time);
      editor::tests::CompareBitmapToBitmap(
          RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
          "typed-cascaded-path-base", editor::tests::PixelmatchIdentityParams());
      EXPECT_EQ(document.source(), sourceBefore);
    }
  }
}

TEST(RendererAnimationPathValue, FrozenPartialToOnlyUsesTypedSplineAtFreezeBoundary) {
  auto document = instantiateSubtree(
      "<path id='p'>"
      "<animate attributeName='d' to='M0 0L16 0L16 16L0 16Z' "
      "dur='2s' repeatCount='0.5' fill='freeze'/></path>",
      parser::SVGParser::Options(), Vector2i(24, 24));
  auto path = document.querySelector("#p")->cast<SVGPathElement>();
  PathBuilder builder;
  builder.moveTo({0, 0}).lineTo({4, 0}).lineTo({4, 4}).lineTo({0, 4}).closePath();
  path.setSpline(builder.build());

  auto expected = instantiateSubtree("<rect width='10' height='10'/>", parser::SVGParser::Options(),
                                     Vector2i(24, 24));
  for (const double time : {5.0, 1.0, 5.0}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "frozen-partial-typed-path", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationPathValue, FrozenPartialToOnlyRebuildsInactivePathLayer) {
  auto document = instantiateSubtree(
      "<path d='M0 0L4 0L4 4L0 4Z'>"
      "<animate attributeName='d' from='M0 0L4 0L4 4L0 4Z' "
      "to='M0 0L12 0L12 12L0 12Z' dur='2s'/>"
      "<animate attributeName='d' to='M0 0L16 0L16 16L0 16Z' "
      "dur='2s' repeatCount='0.5' fill='freeze'/></path>",
      parser::SVGParser::Options(), Vector2i(24, 24));
  auto expected = instantiateSubtree("<rect width='12' height='12'/>", parser::SVGParser::Options(),
                                     Vector2i(24, 24));
  for (const double time : {5.0, 1.0, 5.0}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "frozen-partial-inactive-path", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationPathValue, InvalidCssPathRemainsUnavailableToMotion) {
  for (bool animate : {false, true}) {
    SCOPED_TRACE(animate);
    const std::string animation =
        animate ? "<animate attributeName='d' to='M0 0L24 0' dur='2s'/>" : "";
    auto document = instantiateSubtree(
        "<style>#route {d:invalid;}</style><path id='route' d='M0 0L16 0' stroke='red'>" +
            animation +
            "</path><rect width='4' height='4'><animateMotion dur='2s'>"
            "<mpath href='#route'/></animateMotion></rect>",
        parser::SVGParser::Options(), Vector2i(32, 16));
    auto expected = instantiateSubtree("<rect width='4' height='4'/>", parser::SVGParser::Options(),
                                       Vector2i(32, 16));
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "invalid-css-motion-parity", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationPathValue, CssNoneIsAnEmptyUnderlyingPath) {
  auto document = instantiateSubtree(
      "<style>#p {d:none;}</style><path id='p' d='M0 0L4 0L4 4L0 4Z'>"
      "<animate attributeName='d' to='M0 0L8 0L8 8L0 8Z' dur='2s'/></path>",
      parser::SVGParser::Options(), Vector2i(16, 16));
  for (double time : {0.25, 1.5, 3.0, 0.25}) {
    SCOPED_TRACE(time);
    auto expected = instantiateSubtree(time == 1.5 ? "<rect width='8' height='8'/>" : "",
                                       parser::SVGParser::Options(), Vector2i(16, 16));
    document.setTime(time);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "empty-css-path-base", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationPathValue, MotionUsesSampledTypedPathGeometry) {
  auto document = instantiateSubtree(
      "<defs><path id='route' d='M0 0L4 0'>"
      "<animate attributeName='d' to='M0 0L24 0' dur='2s'/></path></defs>"
      "<rect width='4' height='4'><animateMotion dur='2s'><mpath href='#route'/>"
      "</animateMotion></rect>",
      parser::SVGParser::Options(), Vector2i(32, 16));
  document.querySelector("#route")->cast<SVGPathElement>().setD("M0 0L8 0");
  auto expected = instantiateSubtree("<rect x='8' width='4' height='4'/>",
                                     parser::SVGParser::Options(), Vector2i(32, 16));
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
      "sampled-typed-motion-path", editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
