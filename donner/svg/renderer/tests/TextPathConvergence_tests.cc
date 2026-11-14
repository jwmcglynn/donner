/// @file
/// SVG 2 text-path layout controls independent of the conformance corpus.
#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/css/FontFace.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

RendererBitmap RenderPathScene(std::string_view source) {
  auto document = instantiateSubtree(source, parser::SVGParser::Options(), Vector2i(220, 200));
  return RenderDocumentWithActiveBackend(document);
}

TEST(TextPathConvergence, VerticalStraightPathMatchesOrdinaryVerticalText) {
  for (std::string_view attributes : {"", "textLength='80' lengthAdjust='spacingAndGlyphs'"}) {
    SCOPED_TRACE(attributes);
    const std::string style =
        " font-size='24' writing-mode='vertical-rl' " + std::string(attributes);
    const auto actual = RenderPathScene("<path id='p' d='M80 30V190'/><text" + style +
                                        "><textPath href='#p'>AV</textPath></text>");
    const auto expected = RenderPathScene("<text x='80' y='30'" + style + ">AV</text>");
    ASSERT_THAT(actual.pixels, testing::Not(testing::IsEmpty()));
    ASSERT_THAT(expected.dimensions, testing::Eq(Vector2i(220, 200)));
    const auto transparent = RenderPathScene("");
    EXPECT_NONFATAL_FAILURE(
        editor::tests::CompareBitmapToBitmap(expected, transparent, "vertical_path_visible",
                                             editor::tests::PixelmatchIdentityParams()),
        "pixels differ");
    editor::tests::CompareBitmapToBitmap(actual, expected, "vertical_path_frame",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// Reads the bundled M+ 1p face.
std::shared_ptr<const std::vector<uint8_t>> ReadMplusBytes() {
  std::ifstream file(
      Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts/MPLUS1p-Regular.ttf"),
      std::ios::binary);
  return std::make_shared<const std::vector<uint8_t>>(std::istreambuf_iterator<char>(file),
                                                      std::istreambuf_iterator<char>());
}

/// Renders \p source with \p bytes registered as the `VerticalOriginControl` family.
RendererBitmap RenderWithMplus(std::shared_ptr<const std::vector<uint8_t>> bytes,
                               std::string_view source) {
  auto document = instantiateSubtree(source, parser::SVGParser::Options(), Vector2i(220, 200));
  auto& registry = document.registry();
  auto& manager = registry.ctx().emplace<FontManager>(registry);
  css::FontFace face;
  face.familyName = RcString("VerticalOriginControl");
  css::FontFaceSource fontSource;
  fontSource.kind = css::FontFaceSource::Kind::Data;
  fontSource.payload = std::move(bytes);
  fontSource.trusted = true;
  face.sources.push_back(std::move(fontSource));
  manager.addFontFace(face);
  return RenderDocumentWithActiveBackend(document);
}

/// Upright CJK takes its origin and advance from the font's vertical metrics in both flat text
/// and on a path. For M+ 1p, vmtx places the origins of 日 and 本 860 units below the top of a
/// 1000-unit vertical advance, and each glyph is centered on its 1000-unit horizontal advance.
/// At 25px, where every one of these offsets is exact in 1/64 px, a cell starting at y=30 draws
/// each glyph as horizontal text at x=80-12.5 with baselines 30+21.5 and 55+21.5.
TEST(TextPathConvergence, UprightCjkUsesFontVerticalOriginsOnPathsAndInFlatText) {
  const auto mplus = ReadMplusBytes();
  ASSERT_THAT(*mplus, testing::Not(testing::IsEmpty()));
  constexpr std::string_view kStyle =
      " font-family='VerticalOriginControl' font-size='25' writing-mode='vertical-rl'";
  const auto onPath =
      RenderWithMplus(mplus, "<path id='p' d='M80 30V190'/><text" + std::string(kStyle) +
                                 "><textPath href='#p'>\xe6\x97\xa5\xe6\x9c\xac</textPath></text>");
  const auto flat = RenderWithMplus(
      mplus, "<text x='80' y='30'" + std::string(kStyle) + ">\xe6\x97\xa5\xe6\x9c\xac</text>");
  const auto expected =
      RenderWithMplus(mplus,
                      "<text x='67.5 67.5' y='51.5 76.5' font-family='VerticalOriginControl' "
                      "font-size='25'>\xe6\x97\xa5\xe6\x9c\xac</text>");
  const auto displaced =
      RenderWithMplus(mplus,
                      "<text x='67.5 67.5' y='52.5 77.5' font-family='VerticalOriginControl' "
                      "font-size='25'>\xe6\x97\xa5\xe6\x9c\xac</text>");
  ASSERT_THAT(expected.dimensions, testing::Eq(Vector2i(220, 200)));
  const auto transparent = RenderPathScene("");
  EXPECT_NONFATAL_FAILURE(
      editor::tests::CompareBitmapToBitmap(expected, transparent, "upright_origin_visible",
                                           editor::tests::PixelmatchIdentityParams()),
      "pixels differ");
  EXPECT_NONFATAL_FAILURE(
      editor::tests::CompareBitmapToBitmap(displaced, expected, "upright_origin_displaced",
                                           editor::tests::PixelmatchIdentityParams()),
      "pixels differ");
  editor::tests::CompareBitmapToBitmap(flat, expected, "upright_origin_flat",
                                       editor::tests::PixelmatchIdentityParams());
  editor::tests::CompareBitmapToBitmap(onPath, expected, "upright_origin_path",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(TextPathConvergence, MixedChildrenContinueAtEachGeometricEndpoint) {
  const auto actual = RenderPathScene(
      "<defs><path id='p' d='M10 25H60'/><path id='q' d='M120 85V130'/></defs>"
      "<text font-size='18'><textPath href='#p'>A</textPath>"
      "<tspan fill='green'>V</tspan><textPath href='#q'>B</textPath>W</text>");
  const auto expected = RenderPathScene(
      "<text font-size='18'><textPath path='M10 25H60'>A</textPath>"
      "<tspan x='60' y='25' fill='green'>V</tspan>"
      "<textPath path='M120 85V130'>B</textPath><tspan x='120' y='130'>W</tspan></text>");
  ASSERT_THAT(actual.pixels, testing::Not(testing::IsEmpty()));
  ASSERT_THAT(expected.dimensions, testing::Eq(Vector2i(220, 200)));
  const auto transparent = RenderPathScene("");
  EXPECT_NONFATAL_FAILURE(
      editor::tests::CompareBitmapToBitmap(expected, transparent, "mixed_path_visible",
                                           editor::tests::PixelmatchIdentityParams()),
      "pixels differ");
  editor::tests::CompareBitmapToBitmap(actual, expected, "mixed_path_endpoints",
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
