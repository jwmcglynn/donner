#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "donner/editor/tests/BitmapGoldenCompare.h"
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

}  // namespace
}  // namespace donner::svg
