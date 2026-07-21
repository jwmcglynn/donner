#include <gtest/gtest.h>

#include "donner/svg/renderer/tests/RendererTestUtils.h"

namespace donner::svg {
namespace {

TEST(RendererAsciiTests, RectAscii) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"(
        <rect width="8" height="8" fill="black" />
        )");

  EXPECT_TRUE(generatedAscii.matches(R"(
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        )"));
}

TEST(RendererAsciiTests, TinySkiaMismatchDoesNotFallBackToDefault) {
  if (ActiveRendererBackend() != RendererBackend::TinySkia) {
    GTEST_SKIP() << "TinySkia-specific matcher regression";
  }

  const AsciiImage geodeImage{"geode\n"};
  EXPECT_FALSE(geodeImage.matchBackend().defaultPattern("geode\n").tinySkia("tiny-skia\n"));
}

TEST(RendererAsciiTests, GeodeAlternativeMatchesExactly) {
  if (ActiveRendererBackend() != RendererBackend::Geode) {
    GTEST_SKIP() << "Geode-specific matcher regression";
  }

  const AsciiImage alternativeImage{"alternative\n"};
  EXPECT_TRUE(alternativeImage.matchBackend()
                  .geode("primary\n")
                  .geodeAlternative("alternative\n"));
}

TEST(RendererAsciiTests, TspanPaintOrderMatchesExplicitStrokeThenFillPasses) {
  const AsciiImage actual = RendererTestUtils::renderToAsciiImage(
      R"(
        <text x="4" y="24" font-family="Noto Sans" font-size="24"
              fill="green" stroke="blue" stroke-width="4">
          <tspan x="4" paint-order="stroke fill">H</tspan>
        </text>
        )",
      Vector2i(32, 32));
  const AsciiImage expected = RendererTestUtils::renderToAsciiImage(
      R"(
        <text x="4" y="24" font-family="Noto Sans" font-size="24"
              fill="none" stroke="blue" stroke-width="4"><tspan x="4">H</tspan></text>
        <text x="4" y="24" font-family="Noto Sans" font-size="24"
              fill="green" stroke="none"><tspan x="4">H</tspan></text>
        )",
      Vector2i(32, 32));

  EXPECT_EQ(actual.generated, expected.generated);
}

}  // namespace
}  // namespace donner::svg
