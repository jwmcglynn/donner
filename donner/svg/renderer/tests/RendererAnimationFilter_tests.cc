#include <gmock/gmock.h>
#include <gtest/gtest.h>

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

}  // namespace
}  // namespace donner::svg
