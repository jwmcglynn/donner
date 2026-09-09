#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <tuple>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/shader/tests/CompositingLegacyPixels.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

namespace donner::gpu::shader {
namespace {

using CompositingCase = tests::LegacyCompositingCase;

class FilterCompositingLegacy : public testing::TestWithParam<CompositingCase> {};

TEST_P(FilterCompositingLegacy, MatchesLegacyGpuPixels) {
  const auto [operation, sourceOpacity, destinationOpacity, colorSpace, reference] = GetParam();
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
  EXPECT_THAT(warnings.warnings(), testing::IsEmpty());
  ASSERT_THAT(gpuDocument.hasResult(), testing::IsTrue());
  static const std::shared_ptr<geode::GeodeDevice> device = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(device, testing::NotNull());
  svg::RendererGeode gpuRenderer(device);
  gpuRenderer.draw(gpuDocument.result());
  const svg::RendererBitmap actual = gpuRenderer.takeSnapshot();
  svg::RendererBitmap expected{Vector2i(13, 11), {}, 13 * 4};
  for (size_t pixel = 0; pixel < 13 * 11; ++pixel) {
    expected.pixels.insert(expected.pixels.end(), reference.begin(), reference.end());
  }
  ASSERT_THAT(actual.dimensions, testing::Eq(Vector2i(13, 11)));
  ASSERT_THAT(expected.dimensions, testing::Eq(Vector2i(13, 11)));
  editor::tests::CompareBitmapToBitmap(
      actual, expected,
      std::string(operation) + "_" + sourceOpacity + "_" + destinationOpacity + "_" + colorSpace,
      editor::tests::PixelmatchIdentityParams());
}

INSTANTIATE_TEST_SUITE_P(OperatorsAndInputOrder, FilterCompositingLegacy,
                         testing::ValuesIn(tests::kLegacyCompositingCases));

}  // namespace
}  // namespace donner::gpu::shader
