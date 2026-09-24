#include <gtest/gtest.h>

#include <cstddef>
#include <string_view>
#include <utility>

#include "donner/base/ParseResult.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"

TEST(BcrConsumer, RendersOpaqueSwatchPixels) {
  constexpr std::string_view kSvg = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">
      <rect id="swatch" width="8" height="8" fill="#d33" />
    </svg>
  )svg";

  donner::ParseWarningSink warnings;
  donner::ParseResult<donner::svg::SVGDocument> maybeDocument =
      donner::svg::parser::SVGParser::ParseSVG(kSvg, warnings);
  ASSERT_FALSE(maybeDocument.hasError()) << maybeDocument.error();

  donner::svg::SVGDocument document = std::move(maybeDocument.result());
  ASSERT_TRUE(document.querySelector("#swatch").has_value()) << "expected #swatch";

  donner::svg::Renderer renderer;
  renderer.draw(document);
  ASSERT_EQ(renderer.width(), 8);
  ASSERT_EQ(renderer.height(), 8);

  const donner::svg::RendererBitmap actual = renderer.takeSnapshot();
  // The comparison reads every visible pixel; reject an incomplete snapshot first.
  ASSERT_FALSE(actual.empty());
  ASSERT_EQ(actual.dimensions, donner::Vector2i(8, 8));
  ASSERT_GE(actual.rowBytes, 8u * 4u);
  ASSERT_GE(actual.pixels.size(), 8u * 4u);
  ASSERT_LE(actual.rowBytes, (actual.pixels.size() - 8u * 4u) / 7u);

  donner::svg::RendererBitmap expected;
  expected.dimensions = donner::Vector2i(8, 8);
  expected.rowBytes = actual.rowBytes;
  expected.pixels.resize(actual.pixels.size());
  for (std::size_t row = 0; row < 8; ++row) {
    for (std::size_t column = 0; column < 8; ++column) {
      const std::size_t offset = row * expected.rowBytes + column * 4u;
      expected.pixels[offset] = 0xdd;
      expected.pixels[offset + 1] = 0x33;
      expected.pixels[offset + 2] = 0x33;
      expected.pixels[offset + 3] = 0xff;
    }
  }
  donner::editor::tests::CompareBitmapToBitmap(actual, expected, "bcr-consumer-swatch",
                                               donner::editor::tests::PixelmatchIdentityParams());
}
