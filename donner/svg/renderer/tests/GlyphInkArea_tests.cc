#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <stb/stb_truetype.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"

namespace donner::svg {
namespace {

using testing::DoubleEq;
using testing::DoubleNear;
using testing::Gt;

constexpr std::string_view kFontDirectory = "third_party/resvg-test-suite/fonts";
constexpr std::string_view kFontFile = "third_party/resvg-test-suite/fonts/NotoSans-Regular.ttf";
constexpr std::string_view kText = "Text";
constexpr float kFontSizePx = 160.0f;

/// One opaque black run of \ref kText at \ref kFontSizePx, wholly inside a transparent canvas.
constexpr std::string_view kScene = R"svg(
  <svg xmlns="http://www.w3.org/2000/svg" width="480" height="240">
    <text x="40" y="180" font-family="Noto Sans" font-size="160">Text</text>
  </svg>)svg";

/// The rendered ink may differ from the exact outline area by this fraction of it.
constexpr double kInkRelativeTolerance = 0.001;

std::vector<unsigned char> ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<unsigned char>(std::istreambuf_iterator<char>(file),
                                    std::istreambuf_iterator<char>());
}

/// Twice the signed area that the segment from (x0, y0) to (x1, y1) contributes to its contour.
double TwiceLineArea(double x0, double y0, double x1, double y1) {
  return x0 * y1 - x1 * y0;
}

/// Twice the signed area that a quadratic Bezier contributes to its contour: its chord's
/// contribution plus the region between the chord and the curve, which is exactly 2/3 of the
/// control triangle.
double TwiceQuadraticArea(double x0, double y0, double cx, double cy, double x1, double y1) {
  const double twiceTriangle = (cx - x0) * (y1 - y0) - (cy - y0) * (x1 - x0);
  return TwiceLineArea(x0, y0, x1, y1) + 2.0 / 3.0 * twiceTriangle;
}

/// Exact area enclosed by a glyph's TrueType contours, in square font units. Noto Sans contours
/// do not overlap, so the nonzero-filled area is the magnitude of the summed signed areas.
/// Returns NaN when the glyph has a cubic segment, which a TrueType font does not use.
double GlyphOutlineArea(const stbtt_fontinfo& font, int glyph) {
  stbtt_vertex* vertices = nullptr;
  const int count = stbtt_GetGlyphShape(&font, glyph, &vertices);
  double twiceArea = 0.0;
  double startX = 0.0;
  double startY = 0.0;
  double x = 0.0;
  double y = 0.0;
  bool sawCubic = false;
  for (int i = 0; i < count; ++i) {
    const stbtt_vertex& v = vertices[i];
    switch (v.type) {
      case STBTT_vmove:
        twiceArea += TwiceLineArea(x, y, startX, startY);
        startX = v.x;
        startY = v.y;
        break;
      case STBTT_vline: twiceArea += TwiceLineArea(x, y, v.x, v.y); break;
      case STBTT_vcurve: twiceArea += TwiceQuadraticArea(x, y, v.cx, v.cy, v.x, v.y); break;
      default: sawCubic = true; break;
    }
    x = v.x;
    y = v.y;
  }
  twiceArea += TwiceLineArea(x, y, startX, startY);
  stbtt_FreeShape(&font, vertices);
  return sawCubic ? std::nan("") : std::abs(twiceArea) / 2.0;
}

/// Sum of the coverage (alpha / 255) of every pixel, in square pixels.
double RenderedInk(const RendererBitmap& bitmap) {
  double ink = 0.0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    const uint8_t* row = bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes;
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      ink += row[x * 4 + 3] / 255.0;
    }
  }
  return ink;
}

/// Coverage on the outermost rows and columns; nonzero means the canvas clipped some ink.
double BorderInk(const RendererBitmap& bitmap) {
  double ink = 0.0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    const uint8_t* row = bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes;
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      if (y == 0 || x == 0 || y + 1 == bitmap.dimensions.y || x + 1 == bitmap.dimensions.x) {
        ink += row[x * 4 + 3] / 255.0;
      }
    }
  }
  return ink;
}

// Opaque glyphs cover each pixel by the fraction of its area inside the outline, so the
// rendered ink equals the outline's area. A renderer that draws glyph edges heavier or lighter
// than the outline changes the sum by about the edge offset times the outline's perimeter.
TEST(GlyphInkAreaTest, RenderedInkEqualsExactOutlineArea) {
  const std::vector<unsigned char> fontData =
      ReadFile(Runfiles::instance().Rlocation(std::string(kFontFile)));
  ASSERT_THAT(fontData.size(), Gt(0u)) << kFontFile;
  stbtt_fontinfo font{};
  ASSERT_NE(stbtt_InitFont(&font, fontData.data(), stbtt_GetFontOffsetForIndex(fontData.data(), 0)),
            0)
      << kFontFile;

  double outlineAreaUnits = 0.0;
  for (const char ch : kText) {
    const int glyph = stbtt_FindGlyphIndex(&font, ch);
    ASSERT_NE(glyph, 0) << "no glyph for '" << ch << "'";
    const double area = GlyphOutlineArea(font, glyph);
    ASSERT_THAT(area, Gt(0.0)) << "glyph for '" << ch << "'";
    outlineAreaUnits += area;
  }
  const double pxPerUnit = stbtt_ScaleForMappingEmToPixels(&font, kFontSizePx);
  const double exactArea = outlineAreaUnits * pxPerUnit * pxPerUnit;

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(kScene, warnings);
  ASSERT_FALSE(parsed.hasError()) << parsed.error();
  SVGDocument document = std::move(parsed.result());
  RegisterFontsFromDirectoryForTesting(
      document, Runfiles::instance().Rlocation(std::string(kFontDirectory)));

  const RendererBitmap bitmap = RenderDocumentWithActiveBackend(document);
  ASSERT_FALSE(bitmap.empty()) << ActiveRendererBackendName() << " returned an empty snapshot";
  EXPECT_THAT(BorderInk(bitmap), DoubleEq(0.0)) << "the canvas clips the text";

  const double ink = RenderedInk(bitmap);
  EXPECT_THAT(ink, DoubleNear(exactArea, exactArea * kInkRelativeTolerance))
      << ActiveRendererBackendName() << " draws \"" << kText << "\" with " << ink
      << " px^2 of ink; its outline encloses " << exactArea << " px^2 ("
      << 100.0 * (ink - exactArea) / exactArea << "% difference)";
}

}  // namespace
}  // namespace donner::svg
