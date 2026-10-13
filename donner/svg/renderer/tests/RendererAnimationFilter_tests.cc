#include <gmock/gmock.h>
#include <gtest/gtest.h>

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

}  // namespace
}  // namespace donner::svg
