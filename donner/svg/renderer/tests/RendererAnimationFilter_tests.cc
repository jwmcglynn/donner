#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

SVGDocument FilterDocument(std::string_view primitives, std::string_view animation = {}) {
  return instantiateSubtree(
      "<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='16' height='16' "
      "color-interpolation-filters='sRGB'>" +
          std::string(primitives) +
          "</filter></defs><rect x='4' y='4' width='8' height='8' fill='#804020' "
          "filter='url(#f)'/>" +
          std::string(animation),
      parser::SVGParser::Options(), Vector2i(16, 16));
}

TEST(RendererAnimationFilter, DiscreteControlsMatchStaticValues) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  struct Case {
    const char* name;
    const char* base;
    const char* attribute;
    const char* value;
    const char* expected;
  };
  for (const auto item :
       {Case{"order", "<feConvolveMatrix id='p' order='3' kernelMatrix='1'/>", "order", "1",
             "<feConvolveMatrix order='1' kernelMatrix='1'/>"},
        Case{"target", "<feConvolveMatrix id='p' order='3' kernelMatrix='0 0 0 0 1 0 0 0 0'/>",
             "targetX", "0",
             "<feConvolveMatrix order='3' kernelMatrix='0 0 0 0 1 0 0 0 0' targetX='0'/>"},
        Case{"alpha", "<feConvolveMatrix id='p' order='1' kernelMatrix='0'/>", "preserveAlpha",
             "true", "<feConvolveMatrix order='1' kernelMatrix='0' preserveAlpha='true'/>"},
        Case{"stitch", "<feTurbulence id='p' baseFrequency='.13 .17'/>", "stitchTiles", "stitch",
             "<feTurbulence baseFrequency='.13 .17' stitchTiles='stitch'/>"},
        Case{"channels",
             "<feFlood flood-color='red' result='map'/><feDisplacementMap id='p' "
             "in='SourceGraphic' in2='map' scale='8'/>",
             "xChannelSelector", "G",
             "<feFlood flood-color='red' result='map'/><feDisplacementMap in='SourceGraphic' "
             "in2='map' scale='8' xChannelSelector='G'/>"},
        Case{"spotlight",
             "<feDiffuseLighting><feSpotLight id='p' x='8' y='8' z='10' pointsAtX='8' "
             "pointsAtY='8' pointsAtZ='0'/></feDiffuseLighting>",
             "specularExponent", "4",
             "<feDiffuseLighting><feSpotLight x='8' y='8' z='10' pointsAtX='8' pointsAtY='8' "
             "pointsAtZ='0' specularExponent='4'/></feDiffuseLighting>"}}) {
    SCOPED_TRACE(item.name);
    auto document =
        FilterDocument(item.base, std::string("<set href='#p' attributeName='") + item.attribute +
                                      "' to='" + item.value + "' dur='1s'/>");
    auto expected = FilterDocument(item.expected);
    auto base = FilterDocument(item.base);
    document.setTime(0.5);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected), item.name,
                                         editor::tests::PixelmatchIdentityParams());
    document.setTime(2);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(base),
        std::string(item.name) + "-restored", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationFilter, IntegerInterpolationRoundsInValueSpace) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  auto document =
      FilterDocument("<feTurbulence id='p' baseFrequency='.13 .17' numOctaves='1'/>",
                     "<animate href='#p' attributeName='numOctaves' from='1' to='3' dur='2s'/>");
  auto expected = FilterDocument("<feTurbulence baseFrequency='.13 .17' numOctaves='2'/>");
  document.setTime(0.5);
  editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                       RenderDocumentWithActiveBackend(expected), "integer-half-up",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationFilter, ToOnlyUsesNonzeroComponentDefaults) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  struct Case {
    const char* name;
    const char* base;
    const char* attribute;
    const char* to;
    const char* expected;
  };
  for (const auto item :
       {Case{"drop-offset", "<feDropShadow id='p' dy='0' stdDeviation='0' flood-color='blue'/>",
             "dx", "6", "<feDropShadow dx='4' dy='0' stdDeviation='0' flood-color='blue'/>"},
        Case{"transfer-slope",
             "<feComponentTransfer><feFuncR id='p' type='linear'/></feComponentTransfer>", "slope",
             "0", "<feComponentTransfer><feFuncR type='linear' slope='.5'/></feComponentTransfer>"},
        Case{"lighting",
             "<feDiffuseLighting id='p'><feDistantLight elevation='90'/></feDiffuseLighting>",
             "diffuseConstant", "0",
             "<feDiffuseLighting diffuseConstant='.5'><feDistantLight "
             "elevation='90'/></feDiffuseLighting>"},
        Case{"drop-deviation", "<feDropShadow id='p' dx='0' dy='0'/>", "stdDeviation", "4",
             "<feDropShadow dx='0' dy='0' stdDeviation='3'/>"}}) {
    SCOPED_TRACE(item.name);
    auto document =
        FilterDocument(item.base, std::string("<animate href='#p' attributeName='") +
                                      item.attribute + "' to='" + item.to + "' dur='2s'/>");
    auto expected = FilterDocument(item.expected);
    document.setTime(1);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected), item.name,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationFilter, NumberPairRepeatsItsSingleCoordinate) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  auto document = FilterDocument(
      "<feGaussianBlur id='p'/>",
      "<animate href='#p' attributeName='stdDeviation' from='0 2' to='2' dur='2s'/>");
  auto expected = FilterDocument("<feGaussianBlur stdDeviation='1 2'/>");
  document.setTime(1);
  editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                       RenderDocumentWithActiveBackend(expected), "number-pair",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererAnimationFilter, InputsAndResultNamesAnimateAndRestore) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  struct Case {
    const char* name;
    const char* base;
    const char* attribute;
    const char* to;
    const char* expected;
  };
  for (const auto item :
       {Case{"input",
             "<feFlood flood-color='red' result='red'/><feFlood flood-color='blue' "
             "result='blue'/><feOffset id='p' in='red'/>",
             "in", "blue",
             "<feFlood flood-color='red' result='red'/><feFlood flood-color='blue' "
             "result='blue'/><feOffset in='blue'/>"},
        Case{"input2",
             "<feFlood flood-color='red' result='red'/><feFlood flood-color='blue' "
             "result='blue'/><feBlend id='p' in='SourceGraphic' in2='red' mode='multiply'/>",
             "in2", "blue",
             "<feFlood flood-color='red' result='red'/><feFlood flood-color='blue' "
             "result='blue'/><feBlend in='SourceGraphic' in2='blue' mode='multiply'/>"},
        Case{"result",
             "<feFlood id='p' flood-color='red' result='old'/><feFlood "
             "flood-color='blue'/><feOffset in='new'/>",
             "result", "new",
             "<feFlood flood-color='red' result='new'/><feFlood flood-color='blue'/><feOffset "
             "in='new'/>"}}) {
    SCOPED_TRACE(item.name);
    auto document =
        FilterDocument(item.base, std::string("<set href='#p' attributeName='") + item.attribute +
                                      "' to='" + item.to + "' dur='1s'/>");
    auto expected = FilterDocument(item.expected);
    auto base = FilterDocument(item.base);
    document.setTime(0.5);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected), item.name,
                                         editor::tests::PixelmatchIdentityParams());
    document.setTime(2);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(base),
        std::string(item.name) + "-restored", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RendererAnimationFilter, ToOnlyPreservesImplicitAndTypedInputs) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  for (const bool typed : {false, true}) {
    SCOPED_TRACE(typed);
    auto document =
        FilterDocument("<feFlood flood-color='blue' result='blue'/><feOffset id='p'/>",
                       "<animate href='#p' attributeName='in' to='SourceAlpha' dur='2s'/>");
    if (typed) {
      document.querySelector("#p")->entityHandle().get<components::FilterPrimitiveComponent>().in =
          components::FilterInput(components::FilterStandardInput::SourceGraphic);
    }
    auto expected = FilterDocument(typed ? "<feOffset in='SourceGraphic'/>"
                                         : "<feFlood flood-color='blue'/><feOffset/>");
    document.setTime(0.5);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected),
                                         typed ? "typed-input-base" : "implicit-input-base",
                                         editor::tests::PixelmatchIdentityParams());
    auto final = FilterDocument("<feOffset in='SourceAlpha'/>");
    document.setTime(1.5);
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(final), "to-input",
                                         editor::tests::PixelmatchIdentityParams());
    document.setTime(3);
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        "restored-input", editor::tests::PixelmatchIdentityParams());
  }
}

struct NumericFilterCase {
  std::string name;
  std::string primitive;
  std::string attribute;
  std::string base;
  std::string to;
  std::string midpoint;
};

void PrintTo(const NumericFilterCase& value, std::ostream* os) {
  *os << value.name;
}

std::string NumericFilterPrimitive(const NumericFilterCase& item, std::string_view value) {
  std::string primitive = item.primitive;
  primitive.replace(
      primitive.find('@'), 1,
      value.empty() ? std::string{} : item.attribute + "='" + std::string(value) + "'");
  return primitive;
}

class RendererAnimationNumericFilter : public testing::TestWithParam<NumericFilterCase> {};

TEST_P(RendererAnimationNumericFilter, InterpolationSeekRestorationAndAuthoredEdit) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  const auto& item = GetParam();
  auto document = FilterDocument(
      NumericFilterPrimitive(item, item.base),
      "<animate href='#p' attributeName='" + item.attribute + "' to='" + item.to + "' dur='2s'/>");
  const std::string authoredSource(document.source());
  for (const double time : {1.0, 3.0, 1.0}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    auto expected =
        FilterDocument(NumericFilterPrimitive(item, time == 1.0 ? item.midpoint : item.base));
    editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document),
                                         RenderDocumentWithActiveBackend(expected),
                                         item.name + (time == 1.0 ? "-midpoint" : "-restored"),
                                         editor::tests::PixelmatchIdentityParams());
    EXPECT_EQ(document.source(), authoredSource);
  }

  auto target = document.querySelector("#p");
  ASSERT_THAT(target, testing::Ne(std::nullopt));
  target->setAttribute(xml::XMLQualifiedNameRef(item.attribute), item.to);
  const std::string editedSource(document.source());
  for (const double time : {1.0, 3.0}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    auto expected = FilterDocument(NumericFilterPrimitive(item, item.to));
    editor::tests::CompareBitmapToBitmap(
        RenderDocumentWithActiveBackend(document), RenderDocumentWithActiveBackend(expected),
        item.name + "-authored-edit", editor::tests::PixelmatchIdentityParams());
    EXPECT_EQ(document.source(), editedSource);
  }
}

std::vector<NumericFilterCase> NumericFilterCases() {
  const std::string inputs =
      "<feFlood flood-color='red' flood-opacity='.4' result='red'/>"
      "<feFlood flood-color='blue' flood-opacity='.6' result='blue'/>";
  const std::string composite =
      inputs + "<feComposite id='p' in='red' in2='blue' operator='arithmetic' ";
  return {
      {"GammaAmplitude",
       "<feComponentTransfer><feFuncR id='p' type='gamma' @/></feComponentTransfer>", "amplitude",
       "1", "0", ".5"},
      {"GammaExponent",
       "<feComponentTransfer><feFuncR id='p' type='gamma' @/></feComponentTransfer>", "exponent",
       "1", "3", "2"},
      {"GammaOffset", "<feComponentTransfer><feFuncR id='p' type='gamma' @/></feComponentTransfer>",
       "offset", "0", ".4", ".2"},
      {"LinearIntercept",
       "<feComponentTransfer><feFuncG id='p' type='linear' @/></feComponentTransfer>", "intercept",
       "0", ".4", ".2"},
      {"ArithmeticK1", composite + "k2='.25' k3='.25' @/>", "k1", "0", "1", ".5"},
      {"ArithmeticK2", composite + "k1='.25' k3='.25' @/>", "k2", ".25", ".75", ".5"},
      {"ArithmeticK3", composite + "k1='.25' k2='.25' @/>", "k3", ".25", ".75", ".5"},
      {"ArithmeticK4", composite + "k2='.25' k3='.25' @/>", "k4", "0", ".25", ".125"},
      {"ConvolutionBias", "<feConvolveMatrix id='p' order='1' kernelMatrix='1' @/>", "bias", "0",
       ".4", ".2"},
      {"ConvolutionImplicitDivisor", "<feConvolveMatrix id='p' order='1' kernelMatrix='2' @/>",
       "divisor", "", "6", "4"},
      {"DisplacementScale",
       "<feFlood flood-color='red' result='map'/>"
       "<feDisplacementMap id='p' in='SourceGraphic' in2='map' xChannelSelector='R' "
       "yChannelSelector='G' @/>",
       "scale", "0", "8", "4"},
      {"TurbulenceSeed", "<feTurbulence id='p' baseFrequency='.13 .17' @/>", "seed", "1", "3", "2"},
  };
}

INSTANTIATE_TEST_SUITE_P(NumericFields, RendererAnimationNumericFilter,
                         testing::ValuesIn(NumericFilterCases()),
                         [](const testing::TestParamInfo<NumericFilterCase>& info) {
                           return info.param.name;
                         });

/// \p count space-separated numbers, all zero except \p one at \p index when it is in range.
std::string NumberList(std::size_t count, std::size_t index = 0, std::string_view one = "0") {
  std::string result;
  for (std::size_t i = 0; i < count; ++i) {
    result += i == 0 ? "" : " ";
    result += i == index ? std::string(one) : std::string("0");
  }
  return result;
}

/// A value set by an animation, and the same value authored statically.
struct AuthoredValueCase {
  std::string name;       //!< Case label.
  std::string base;       //!< Primitives before the animation, with the target `#p`.
  std::string attribute;  //!< The animated attribute.
  std::string value;      //!< The animated value.
  std::string authored;   //!< Primitives authoring \c value statically.
  /// Whether the case convolves with more than 25 kernel values, which Geode renders as transparent
  /// black (issue #2101), so its base and authored renderings match there.
  bool largeKernel = false;
};

void PrintTo(const AuthoredValueCase& value, std::ostream* os) {
  *os << value.name;
}

class RendererAnimationFilterAuthoredValue : public testing::TestWithParam<AuthoredValueCase> {};

// An animated filter value renders exactly as the same value authored, including a list longer
// than the general animation number cap, a value over a structural limit, which fails closed, and
// a value a renderer bounds.
TEST_P(RendererAnimationFilterAuthoredValue, MatchesTheAuthoredValue) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  const AuthoredValueCase& item = GetParam();
  auto authored = FilterDocument(item.authored);
  auto base = FilterDocument(item.base);
  const RendererBitmap expected = RenderDocumentWithActiveBackend(authored);
  if (!item.largeKernel || ActiveRendererBackend() != RendererBackend::Geode) {
    ASSERT_NE(RenderDocumentWithActiveBackend(base).pixels, expected.pixels)
        << "the authored value must render differently from the base value";
  }

  auto document = FilterDocument(item.base, "<set href='#p' attributeName='" + item.attribute +
                                                "' to='" + item.value + "' dur='1s'/>");
  document.setTime(0.5);
  editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document), expected,
                                       item.name, editor::tests::PixelmatchIdentityParams());
}

std::vector<AuthoredValueCase> AuthoredValueCases() {
  const auto convolve = [](std::string_view order, std::string_view kernel, bool target) {
    return std::string("<feConvolveMatrix") + (target ? " id='p'" : "") + " order='" +
           std::string(order) + "' kernelMatrix='" + std::string(kernel) + "'/>";
  };
  const auto table = [](std::string_view values, bool target) {
    return std::string("<feComponentTransfer><feFuncR") + (target ? " id='p'" : "") +
           " type='table' tableValues='" + std::string(values) + "'/></feComponentTransfer>";
  };
  const auto primitive = [](std::string_view tag, std::string_view attributes,
                            std::string_view attribute, std::string_view value, bool target) {
    return "<" + std::string(tag) + (target ? " id='p' " : " ") + std::string(attributes) + " " +
           std::string(attribute) + "='" + std::string(value) + "'/>";
  };
  std::vector<AuthoredValueCase> cases;
  const auto add = [&](std::string name, std::string tag, std::string attributes,
                       std::string attribute, std::string baseValue, std::string value) {
    cases.push_back({std::move(name), primitive(tag, attributes, attribute, baseValue, true),
                     attribute, value, primitive(tag, attributes, attribute, value, false)});
  };

  const std::string center = NumberList(81, 40, "1");
  const std::string corner = NumberList(81, 0, "1");
  cases.push_back({"Kernel9x9", convolve("9", center, true), "kernelMatrix", corner,
                   convolve("9", corner, false), /*largeKernel=*/true});
  for (const std::size_t count : {std::size_t{4097}, std::size_t{100000}}) {
    const std::string kernel = NumberList(count);
    cases.push_back({"KernelOverLimit" + std::to_string(count), convolve("1", "1", true),
                     "kernelMatrix", kernel, convolve("1", kernel, false)});
  }
  cases.push_back({"OrderOverLimit", convolve("1", "1", true), "order", "65",
                   "<feConvolveMatrix order='65' kernelMatrix='1'/>"});
  cases.push_back({"OrderFractionOverLimit", convolve("1", "1", true), "order", "64.4",
                   "<feConvolveMatrix order='64.4' kernelMatrix='1'/>"});
  const std::string longTable = NumberList(100, 0, "1");
  cases.push_back(
      {"Table100", table("0 1", true), "tableValues", longTable, table(longTable, false)});
  const std::string overTable = NumberList(1025, 0, "1");
  cases.push_back(
      {"TableOverLimit", table("0 1", true), "tableValues", overTable, table(overTable, false)});
  add("ColorMatrixValuesOverLimit", "feColorMatrix", "type='saturate'", "values", ".25",
      NumberList(100, 0, "1"));
  add("TurbulenceOctavesOverLimit", "feTurbulence", "baseFrequency='.2'", "numOctaves", "1", "17");
  add("TurbulenceFrequencyOverLimit", "feTurbulence", "", "baseFrequency", ".2", "2000");
  add("TurbulenceSeedOverLimit", "feTurbulence", "baseFrequency='.2'", "seed", "1", "3000000000");
  add("BlurDeviationOverLimit", "feGaussianBlur", "", "stdDeviation", "1", "300");
  add("OffsetOverLimit", "feOffset", "", "dx", "1", "5000");
  add("MorphologyRadiusOverLimit", "feMorphology", "operator='dilate'", "radius", "1", "300");
  return cases;
}

// An interpolated order above the limit renders like the same authored order, which fails
// closed, instead of rounding down to a 64x64 kernel.
TEST(RendererAnimationFilter, InterpolatedOrderAboveTheLimitMatchesAuthored) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  auto authored = FilterDocument("<feConvolveMatrix order='64.4' kernelMatrix='1'/>");
  auto base = FilterDocument("<feConvolveMatrix id='p' order='1' kernelMatrix='1'/>");
  const RendererBitmap expected = RenderDocumentWithActiveBackend(authored);
  ASSERT_NE(RenderDocumentWithActiveBackend(base).pixels, expected.pixels)
      << "the authored order must render differently from the base order";

  auto document =
      FilterDocument("<feConvolveMatrix id='p' order='1' kernelMatrix='1'/>",
                     "<animate href='#p' attributeName='order' values='64.4;64.4' dur='1s'/>");
  document.setTime(0.5);
  editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document), expected,
                                       "interpolated_order_above_limit",
                                       editor::tests::PixelmatchIdentityParams());
}

INSTANTIATE_TEST_SUITE_P(Values, RendererAnimationFilterAuthoredValue,
                         testing::ValuesIn(AuthoredValueCases()),
                         [](const testing::TestParamInfo<AuthoredValueCase>& info) {
                           return info.param.name;
                         });

}  // namespace
}  // namespace donner::svg
