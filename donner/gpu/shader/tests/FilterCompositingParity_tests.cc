#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererTinySkia.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeFilterEngine.h"

namespace donner::gpu::shader {
namespace {

TEST(FilterPrecisionParity, DropShadowHalfOffsetsMatchSoftwareRenderer) {
  const char* offsets[][2] = {
      {"0.5", "0"}, {"-0.5", "0"}, {"0", "0.5"}, {"0", "-0.5"}, {"2.5", "-2.5"}};
  static const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  size_t index = 0;
  for (const auto& offset : offsets) {
    SCOPED_TRACE(testing::Message() << "dx=" << offset[0] << " dy=" << offset[1]);
    const std::string source =
        std::string(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="13" height="11">
      <defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="13" height="11"
      color-interpolation-filters="sRGB"><feDropShadow stdDeviation="0" flood-color="blue" dx=")svg") +
        offset[0] + R"svg(" dy=")svg" + offset[1] + R"svg("/></filter></defs>
      <rect x="4" y="4" width="2" height="2" fill="red" filter="url(#f)"/></svg>)svg";
    ParseWarningSink warnings;
    auto gpuDocument = svg::parser::SVGParser::ParseSVG(source, warnings);
    auto cpuDocument = svg::parser::SVGParser::ParseSVG(source, warnings);
    ASSERT_THAT(gpuDocument.hasResult(), testing::IsTrue());
    ASSERT_THAT(cpuDocument.hasResult(), testing::IsTrue());
    ASSERT_THAT(warnings.warnings(), testing::IsEmpty());
    svg::RendererGeode gpuRenderer(device);
    svg::RendererTinySkia cpuRenderer;
    gpuRenderer.draw(gpuDocument.result());
    cpuRenderer.draw(cpuDocument.result());
    editor::tests::CompareBitmapToBitmap(gpuRenderer.takeSnapshot(), cpuRenderer.takeSnapshot(),
                                         "drop_shadow_half_" + std::to_string(index++),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(FilterPrecisionParity, PixelOffsetsKeepDoublePrecisionUntilRounding) {
  static const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  size_t index = 0;
  for (bool shadow : {false, true}) {
    for (const char* offset : {"0.499999999", "-0.499999999", "2.499999999", "-2.499999999"}) {
      for (bool vertical : {false, true}) {
        SCOPED_TRACE(testing::Message()
                     << "shadow=" << shadow << " offset=" << offset << " vertical=" << vertical);
        const std::string primitive =
            shadow ? "<feDropShadow stdDeviation=\"0\" flood-color=\"blue\"" : "<feOffset";
        const std::string source =
            std::string(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="13" height="11">
          <defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="13" height="11"
          color-interpolation-filters="sRGB">)svg") +
            primitive + " dx=\"" + (vertical ? "0" : offset) + "\" dy=\"" +
            (vertical ? offset : "0") + R"svg("/></filter></defs>
          <rect x="4" y="4" width="2" height="2" fill="red" filter="url(#f)"/></svg>)svg";
        ParseWarningSink warnings;
        auto gpuDocument = svg::parser::SVGParser::ParseSVG(source, warnings);
        auto cpuDocument = svg::parser::SVGParser::ParseSVG(source, warnings);
        ASSERT_THAT(gpuDocument.hasResult(), testing::IsTrue());
        ASSERT_THAT(cpuDocument.hasResult(), testing::IsTrue());
        ASSERT_THAT(warnings.warnings(), testing::IsEmpty());
        svg::RendererGeode gpuRenderer(device);
        svg::RendererTinySkia cpuRenderer;
        gpuRenderer.draw(gpuDocument.result());
        cpuRenderer.draw(cpuDocument.result());
        editor::tests::CompareBitmapToBitmap(gpuRenderer.takeSnapshot(), cpuRenderer.takeSnapshot(),
                                             "offset_double_precision_" + std::to_string(index++),
                                             editor::tests::PixelmatchIdentityParams());
      }
    }
  }
}

using CompositingCase = std::tuple<const char*, const char*, const char*, const char*>;

class FilterCompositingParity : public testing::TestWithParam<CompositingCase> {};

TEST_P(FilterCompositingParity, GeneratedProgramMatchesSoftwareRenderer) {
  const auto [operation, sourceOpacity, destinationOpacity, colorSpace] = GetParam();
  const std::string primitive =
      std::string(operation).starts_with("merge")
          ? std::string("<feMerge><feMergeNode in=\"") +
                (std::string(operation) == "merge" ? "destination\"/><feMergeNode in=\"source"
                                                   : "source\"/><feMergeNode in=\"destination") +
                "\"/></feMerge>"
          : std::string("<feComposite in=\"source\" in2=\"destination\" operator=\"") + operation +
                "\" k1=\"0.25\" k2=\"0.5\" k3=\"0.5\" k4=\"0.25\"/>";
  const std::string source =
      std::string(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="13" height="11">
      <defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="13" height="11"
      color-interpolation-filters=")svg") +
      colorSpace + R"svg(">
      <feFlood flood-color="red" flood-opacity=")svg" +
      sourceOpacity + R"svg(" result="source"/>
      <feFlood flood-color="blue" flood-opacity=")svg" +
      destinationOpacity + R"svg(" result="destination"/>)svg" + primitive + R"svg(</filter></defs>
      <rect width="13" height="11" filter="url(#f)"/></svg>)svg";
  ParseWarningSink warnings;
  auto gpuDocument = svg::parser::SVGParser::ParseSVG(source, warnings);
  auto cpuDocument = svg::parser::SVGParser::ParseSVG(source, warnings);
  EXPECT_THAT(warnings.warnings(), testing::IsEmpty());
  ASSERT_THAT(gpuDocument.hasResult(), testing::IsTrue());
  ASSERT_THAT(cpuDocument.hasResult(), testing::IsTrue());
  static const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  svg::RendererGeode gpuRenderer(device);
  svg::RendererTinySkia cpuRenderer;
  gpuRenderer.draw(gpuDocument.result());
  cpuRenderer.draw(cpuDocument.result());
  const svg::RendererBitmap actual = gpuRenderer.takeSnapshot();
  const svg::RendererBitmap expected = cpuRenderer.takeSnapshot();
  ASSERT_THAT(actual.dimensions, testing::Eq(Vector2i(13, 11)));
  ASSERT_THAT(expected.dimensions, testing::Eq(Vector2i(13, 11)));
  editor::tests::CompareBitmapToBitmap(
      actual, expected,
      std::string(operation) + "_" + sourceOpacity + "_" + destinationOpacity + "_" + colorSpace,
      editor::tests::PixelmatchIdentityParams());
}

void ExpectFilterChainMatches(const std::string& primitives, const char* caseName) {
  const std::string source =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="13" height="11">
      <defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="13" height="11">
      <feFlood flood-color="#3973ad" flood-opacity="0.7" result="seed"/>)svg" +
      primitives + R"svg(</filter></defs><rect width="13" height="11" filter="url(#f)"/></svg>)svg";
  ParseWarningSink warnings;
  auto gpuDocument = svg::parser::SVGParser::ParseSVG(source, warnings);
  auto cpuDocument = svg::parser::SVGParser::ParseSVG(source, warnings);
  ASSERT_THAT(gpuDocument.hasResult(), testing::IsTrue());
  ASSERT_THAT(cpuDocument.hasResult(), testing::IsTrue());
  ASSERT_THAT(warnings.warnings(), testing::IsEmpty());
  svg::RendererGeode gpuRenderer;
  svg::RendererTinySkia cpuRenderer;
  gpuRenderer.draw(gpuDocument.result());
  cpuRenderer.draw(cpuDocument.result());
  const svg::RendererBitmap actual = gpuRenderer.takeSnapshot();
  const svg::RendererBitmap expected = cpuRenderer.takeSnapshot();
  ASSERT_THAT(actual.dimensions, testing::Eq(Vector2i(13, 11)));
  ASSERT_THAT(expected.dimensions, testing::Eq(Vector2i(13, 11)));
  editor::tests::CompareBitmapToBitmap(actual, expected, caseName,
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(FilterChainPrecision, FinalDropShadowResolvePreservesLinearColorAndExplicitClip) {
  ExpectFilterChainMatches(R"(<feDropShadow dx="0" dy="0" stdDeviation="0"
      flood-color="#a75321" flood-opacity="0.4"/>)",
                           "fused_drop_shadow_resolve");
  ExpectFilterChainMatches(R"(<feDropShadow dx="0" dy="0" stdDeviation="0"
      flood-color="#a75321" flood-opacity="0.4" x="2" y="3" width="7" height="5"/>)",
                           "drop_shadow_explicit_final_clip");
}

TEST(FilterChainPrecision, Dpr2FullViewportCompositingFitsTheExistingMemoryCap) {
  const std::string source = R"svg(<svg xmlns="http://www.w3.org/2000/svg"
      width="2000" height="1600" viewBox="0 0 1000 800">
    <defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="1000" height="800">
      <feFlood flood-color="#3973ad" flood-opacity="0.7" result="a"/>
      <feFlood flood-color="#a75321" flood-opacity="0.4" result="b"/>
      <feComposite in="a" in2="b" operator="arithmetic" k2="0.6" k3="0.4"/>
    </filter></defs><rect width="1000" height="800" filter="url(#f)"/>
  </svg>)svg";
  ParseWarningSink warnings;
  auto gpuDocument = svg::parser::SVGParser::ParseSVG(source, warnings);
  std::string reference = source;
  reference.replace(reference.find("width=\"2000\" height=\"1600\""),
                    std::string("width=\"2000\" height=\"1600\"").size(),
                    "width=\"1\" height=\"1\"");
  auto cpuDocument = svg::parser::SVGParser::ParseSVG(reference, warnings);
  ASSERT_TRUE(gpuDocument.hasResult());
  ASSERT_TRUE(cpuDocument.hasResult());
  ASSERT_THAT(warnings.warnings(), testing::IsEmpty());
  std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_TRUE(device);
  svg::RendererGeode gpuRenderer(device);
  svg::RendererTinySkia cpuRenderer;
  gpuRenderer.draw(gpuDocument.result());
  cpuRenderer.draw(cpuDocument.result());
  const auto memory = device->filterEngine().lastExecutionMemory();
  EXPECT_GT(memory.tileExecutions, 1u);
  EXPECT_LT(memory.total() + 2000u * 1600u * 4, 128u * 1024u * 1024u);
  const auto actual = gpuRenderer.takeSnapshot();
  const auto referencePixel = cpuRenderer.takeSnapshot();
  ASSERT_EQ(referencePixel.dimensions, Vector2i(1, 1));
  ASSERT_EQ(actual.dimensions, Vector2i(2000, 1600));
  // Flood-only arithmetic is spatially uniform; construct the full reference from its CPU pixel.
  svg::RendererBitmap expected;
  expected.dimensions = actual.dimensions;
  expected.rowBytes = 2000 * 4;
  expected.alphaType = referencePixel.alphaType;
  expected.pixels.resize(expected.rowBytes * 1600);
  for (size_t offset = 0; offset < expected.pixels.size(); offset += 4) {
    std::copy_n(referencePixel.pixels.begin(), 4, expected.pixels.begin() + offset);
  }
  editor::tests::CompareBitmapToBitmap(actual, expected, "dpr2_full_viewport_compositing",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(FilterChainPrecision, ComponentTransferGammaGuardsAndEmptyTablesMatchCpu) {
  ExpectFilterChainMatches(R"svg(
    <feComponentTransfer color-interpolation-filters="sRGB">
      <feFuncR type="gamma" amplitude="0" exponent="-2" offset="0.3"/>
      <feFuncG type="gamma" amplitude="0.4" exponent="0" offset="0.1"/>
      <feFuncB type="table" tableValues=""/>
      <feFuncA type="discrete" tableValues=""/>
    </feComponentTransfer>
    <feComponentTransfer color-interpolation-filters="sRGB">
      <feFuncR type="gamma" amplitude="0.7" exponent="2" offset="0.03"/>
      <feFuncG type="table" tableValues="0.4"/>
      <feFuncB type="discrete" tableValues="0.6"/>
    </feComponentTransfer>)svg",
                           "component_gamma_empty_tables");
}

TEST(FilterChainPrecision, ComponentTransferMaximumPackedTablesKeepChannelsDistinct) {
  std::string operation = "<feComponentTransfer color-interpolation-filters=\"sRGB\">";
  const char* names[] = {"R", "G", "B", "A"};
  const char* values[] = {"0.2", "0.4", "0.6", "0.8"};
  for (size_t channel = 0; channel < 4; ++channel) {
    operation += std::string("<feFunc") + names[channel] + " type=\"table\" tableValues=\"";
    for (size_t index = 0; index < 1024; ++index) {
      operation += std::string(values[channel]) + " ";
    }
    operation += "\"/>";
  }
  operation += "</feComponentTransfer>";
  ExpectFilterChainMatches(operation + operation, "component_maximum_packed_tables");
}

class FilterBlendParity : public testing::TestWithParam<const char*> {};

TEST_P(FilterBlendParity, StraightChannelsAndAlphaBoundariesMatchCpu) {
  for (const char* opacity : {"0", "0.5", "1"}) {
    SCOPED_TRACE(opacity);
    const std::string primitives = std::string(R"svg(
      <feFlood flood-color="#bf4080" flood-opacity=")svg") +
                                   opacity + R"svg(" result="source"/>
      <feBlend in="source" in2="seed" color-interpolation-filters="sRGB" mode=")svg" +
                                   GetParam() + R"svg("/>)svg";
    ExpectFilterChainMatches(primitives, "blend_channels_alpha");
  }
}

TEST_P(FilterBlendParity, TiedChannelsAndGrayMatchCpu) {
  const std::array<std::pair<const char*, const char*>, 4> colors{{{"#4040bf", "#bfbf40"},
                                                                   {"#40bf40", "#bf40bf"},
                                                                   {"#bf4040", "#40bfbf"},
                                                                   {"#808080", "#4040bf"}}};
  for (const auto& [source, backdrop] : colors) {
    SCOPED_TRACE(testing::Message() << "source=" << source << " backdrop=" << backdrop);
    const std::string primitives =
        std::string(R"svg(<feFlood flood-color=")svg") + backdrop +
        R"svg(" flood-opacity="0.7" result="backdrop"/><feFlood flood-color=")svg" + source +
        R"svg(" flood-opacity="0.5" result="source"/><feBlend in="source" in2="backdrop"
          color-interpolation-filters="sRGB" mode=")svg" +
        GetParam() + R"svg("/>)svg";
    ExpectFilterChainMatches(primitives, "blend_tied_channels");
  }
}

INSTANTIATE_TEST_SUITE_P(StandardModes, FilterBlendParity,
                         testing::Values("normal", "multiply", "screen", "darken", "lighten",
                                         "overlay", "color-dodge", "color-burn", "hard-light",
                                         "soft-light", "difference", "exclusion", "hue",
                                         "saturation", "color", "luminosity"));

TEST(FilterChainPrecision, LinearFunctionsKeepFractionalValuesAcrossSeveralNodes) {
  const std::string operation = R"svg(<feComponentTransfer color-interpolation-filters="sRGB">
    <feFuncR type="linear" slope="0.8" intercept="0.03"/>
    <feFuncG type="linear" slope="0.7" intercept="0.02"/>
    <feFuncB type="linear" slope="0.9" intercept="0.01"/>
  </feComponentTransfer>)svg";
  ExpectFilterChainMatches(operation + operation + operation + operation, "linear_function_chain");
}

TEST(FilterChainPrecision, TableAndDiscreteFunctionsKeepTheirSpecifiedBreakpoints) {
  ExpectFilterChainMatches(R"svg(
    <feComponentTransfer color-interpolation-filters="sRGB">
      <feFuncR type="table" tableValues="0.01 0.25 0.42 0.99"/>
      <feFuncG type="discrete" tableValues="0.05 0.35 0.85"/>
      <feFuncB type="table" tableValues="0.07 0.73"/>
    </feComponentTransfer>
    <feComposite in2="seed" operator="arithmetic" k1="0.05" k2="0.8" k3="0.2" k4="0.01"
      color-interpolation-filters="sRGB"/>)svg",
                           "table_function_chain");
}

TEST(FilterChainPrecision, GammaFunctionsHandleZeroExponentAndTransparentChannels) {
  ExpectFilterChainMatches(R"svg(
    <feComponentTransfer color-interpolation-filters="sRGB">
      <feFuncR type="gamma" amplitude="0.8" exponent="1.8" offset="0.02"/>
      <feFuncG type="linear" slope="0"/>
      <feFuncB type="gamma" amplitude="0" exponent="-1" offset="0.4"/>
    </feComponentTransfer>
    <feComponentTransfer color-interpolation-filters="sRGB">
      <feFuncG type="gamma" amplitude="0.6" exponent="0" offset="0.02"/>
      <feFuncA type="gamma" exponent="0"/>
    </feComponentTransfer>)svg",
                           "gamma_function_chain");
}

TEST(FilterChainPrecision, NamedInputsCrossColorSpacesWithoutLosingTheirOriginalValues) {
  ExpectFilterChainMatches(R"svg(
    <feColorMatrix type="saturate" values="0.65" color-interpolation-filters="linearRGB"
      result="desaturated"/>
    <feComposite in="seed" in2="desaturated" operator="arithmetic"
      k1="0.05" k2="0.8" k3="0.2" k4="0.01" color-interpolation-filters="sRGB"/>
    <feMerge color-interpolation-filters="linearRGB">
      <feMergeNode in="desaturated"/><feMergeNode/><feMergeNode in="seed"/>
    </feMerge>)svg",
                           "mixed_color_space_chain");
}

TEST(FilterChainPrecision, PixelMoversPreserveAFilteredValueAndItsColorSpace) {
  ExpectFilterChainMatches(R"svg(
    <feComponentTransfer color-interpolation-filters="linearRGB">
      <feFuncR type="linear" slope="0.8" intercept="0.03"/>
      <feFuncG type="linear" slope="0.7" intercept="0.02"/>
    </feComponentTransfer>
    <feOffset dx="1" dy="-1"/>
    <feMorphology operator="dilate" radius="1" color-interpolation-filters="linearRGB"/>
    <feComposite in2="seed" operator="over" color-interpolation-filters="sRGB"/>)svg",
                           "pixel_mover_chain");
}

TEST(FilterChainPrecision, ImageUploadsRemainDistinctBeforeSubmission) {
  constexpr const char* kRed =
      "data:image/"
      "png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/"
      "iZk9HQAAAABJRU5ErkJggg==";
  constexpr const char* kBlue =
      "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgYPj/"
      "HwADAgH/5ncLrgAAAABJRU5ErkJggg==";
  for (const bool transparentSecond : {false, true}) {
    SCOPED_TRACE(transparentSecond);
    const std::string second =
        transparentSecond
            ? R"svg(<feImage result="blue"/>)svg"
            : std::string(R"svg(<feImage href=")svg") + kBlue +
                  R"svg(" preserveAspectRatio="none" image-rendering="pixelated" result="blue"/>)svg";
    const std::string images =
        std::string(R"svg(<feImage href=")svg") + kRed +
        R"svg(" preserveAspectRatio="none" image-rendering="pixelated" result="red"/>)svg" +
        second +
        R"svg(<feComposite in="red" in2="blue" operator="arithmetic" k2="0.5" k3="0.5"
                 color-interpolation-filters="sRGB"/>)svg";
    ExpectFilterChainMatches(
        images, transparentSecond ? "image_upload_then_transparent" : "distinct_image_uploads");
  }
}

TEST(FilterChainPrecision, RecycledStorageCannotReuseAnEarlierValuesColorConversion) {
  ExpectFilterChainMatches(R"svg(
    <feComposite in="seed" in2="seed" operator="arithmetic" k2="0.5" k3="0.5"
      color-interpolation-filters="linearRGB" result="kept"/>
    <feFlood flood-color="#286c91" flood-opacity="0.6" result="fresh"/>
    <feMerge color-interpolation-filters="linearRGB">
      <feMergeNode in="kept"/><feMergeNode in="fresh"/>
    </feMerge>)svg",
                           "recycled_color_conversion");
}

TEST(FilterChainPrecision, ABranchedValueKeepsBothRepresentationsUntilItsFinalConsumer) {
  ExpectFilterChainMatches(R"svg(
    <feComponentTransfer in="seed" color-interpolation-filters="linearRGB" result="branch">
      <feFuncR type="linear" slope="0.7" intercept="0.02"/>
    </feComponentTransfer>
    <feComposite in="branch" in2="seed" operator="arithmetic" k2="0.4" k3="0.6"
      color-interpolation-filters="sRGB" result="mixed"/>
    <feFlood flood-color="#286c91" flood-opacity="0.6"/>
    <feMerge color-interpolation-filters="linearRGB">
      <feMergeNode in="mixed"/><feMergeNode/><feMergeNode in="branch"/>
    </feMerge>)svg",
                           "branched_color_lifetime");
}

TEST(FilterCompositingRefusal, ARefusedFilterPreservesTheParentLayer) {
  ParseWarningSink warnings;
  auto background = svg::parser::SVGParser::ParseSVG(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="13" height="11">
        <rect width="13" height="11" fill="magenta"/>
      </svg>)svg",
                                                     warnings);
  auto filtered = svg::parser::SVGParser::ParseSVG(R"svg(
      <svg xmlns="http://www.w3.org/2000/svg" width="13" height="11">
        <defs><filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="13" height="11"
            color-interpolation-filters="sRGB">
          <feFlood flood-color="blue" result="prior"/>
          <feMerge><feMergeNode in="prior"/><feMergeNode in="prior"/></feMerge>
        </filter></defs>
        <rect width="13" height="11" fill="magenta"/>
        <rect width="13" height="11" filter="url(#f)"/>
      </svg>)svg",
                                                   warnings);
  ASSERT_THAT(background.hasResult(), testing::IsTrue());
  ASSERT_THAT(filtered.hasResult(), testing::IsTrue());
  ASSERT_THAT(warnings.warnings(), testing::IsEmpty());
  svg::RendererGeode renderer;
  renderer.draw(background.result());
  const svg::RendererBitmap expected = renderer.takeSnapshot();
  ASSERT_THAT(expected.dimensions, testing::Eq(Vector2i(13, 11)));
  // Admission refuses the forecast without poisoning capacity needed by the parent.
  renderer.setSurfaceBudgetForTesting(4, svg::RendererSurfaceBudget::kMaximumBytes);
  renderer.draw(filtered.result());
  EXPECT_THAT(renderer.resourceStats().filterBudgetRejected, testing::IsTrue());
  EXPECT_THAT(renderer.resourceStats().surfaceBudgetRejected, testing::IsFalse());
  const svg::RendererBitmap actual = renderer.takeSnapshot();
  editor::tests::CompareBitmapToBitmap(actual, expected, "refused_filter_parent",
                                       editor::tests::PixelmatchIdentityParams());
}
INSTANTIATE_TEST_SUITE_P(OperatorsAndInputOrder, FilterCompositingParity,
                         testing::Combine(testing::Values("over", "in", "out", "atop", "xor",
                                                          "lighter", "arithmetic", "merge",
                                                          "merge-reversed"),
                                          testing::Values("0", "0.5", "1"),
                                          testing::Values("0", "0.5", "1"),
                                          testing::Values("sRGB", "linearRGB")));

}  // namespace
}  // namespace donner::gpu::shader
