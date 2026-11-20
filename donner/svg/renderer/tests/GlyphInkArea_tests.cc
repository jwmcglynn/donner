#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <stb/stb_truetype.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/fonts/SfntUtils.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"

namespace donner::svg {
namespace {

using testing::DoubleEq;
using testing::DoubleNear;
using testing::Gt;
using testing::Optional;

constexpr std::string_view kFontDirectory = "third_party/resvg-test-suite/fonts";
constexpr std::string_view kFontFile = "third_party/resvg-test-suite/fonts/NotoSans-Regular.ttf";
constexpr std::string_view kText = "Text";
constexpr double kFontSizePx = 160.0;

/// One opaque black run of \ref kText at \ref kFontSizePx, wholly inside a transparent canvas.
constexpr std::string_view kScene = R"svg(
  <svg xmlns="http://www.w3.org/2000/svg" width="480" height="240">
    <text x="40" y="180" font-family="Noto Sans" font-size="160">Text</text>
  </svg>)svg";

/// The rendered ink may differ from the exact outline area by this fraction of it: 10 px^2 here,
/// the ink of a uniform 0.006 px offset along the 1,671 px outline. Measured deviations are at
/// most 0.03%; drawing every edge 0.27 px outside the outline adds 4.4%.
constexpr double kInkRelativeTolerance = 0.001;

/// Bounds-checked big-endian reader over font table bytes; reads past the end yield zero and
/// clear \ref ok.
class ByteReader {
public:
  explicit ByteReader(std::span<const uint8_t> bytes, std::size_t offset = 0)
      : bytes_(bytes), ok_(offset <= bytes.size()), pos_(ok_ ? offset : 0) {}

  bool ok() const { return ok_; }

  uint8_t u8() { return require(1) ? bytes_[pos_++] : 0; }

  uint16_t u16() {
    if (!require(2)) {
      return 0;
    }
    const uint16_t value = fonts::ReadBe16(bytes_.data() + pos_);
    pos_ += 2;
    return value;
  }

  int16_t s16() { return static_cast<int16_t>(u16()); }

  uint32_t u32() {
    if (!require(4)) {
      return 0;
    }
    const uint32_t value = fonts::ReadBe32(bytes_.data() + pos_);
    pos_ += 4;
    return value;
  }

  void skip(std::size_t count) {
    if (require(count)) {
      pos_ += count;
    }
  }

private:
  bool require(std::size_t count) {
    ok_ = ok_ && bytes_.size() - pos_ >= count;
    return ok_;
  }

  std::span<const uint8_t> bytes_;
  bool ok_;
  std::size_t pos_;
};

/// One TrueType outline point in font units.
struct OutlinePoint {
  double x = 0.0;
  double y = 0.0;
  bool onCurve = true;
};

using Contour = std::vector<OutlinePoint>;

/// The tables of a TrueType font that locate and describe glyph outlines.
struct GlyphTables {
  std::span<const uint8_t> head;
  std::span<const uint8_t> loca;
  std::span<const uint8_t> glyf;
};

std::optional<GlyphTables> FindGlyphTables(std::span<const uint8_t> font) {
  const auto head = fonts::FindSfntTable(font, "head");
  const auto loca = fonts::FindSfntTable(font, "loca");
  const auto glyf = fonts::FindSfntTable(font, "glyf");
  if (!head || !loca || !glyf) {
    return std::nullopt;
  }
  return GlyphTables{*head, *loca, *glyf};
}

/// The `glyf` bytes of \p glyph, located through `loca` in the format `head` selects.
std::optional<std::span<const uint8_t>> GlyphData(const GlyphTables& tables, int glyph) {
  ByteReader head(tables.head, 50);
  const bool longOffsets = head.s16() != 0;
  ByteReader loca(tables.loca, static_cast<std::size_t>(glyph) * (longOffsets ? 4u : 2u));
  const uint32_t start = longOffsets ? loca.u32() : 2u * loca.u16();
  const uint32_t end = longOffsets ? loca.u32() : 2u * loca.u16();
  if (!head.ok() || !loca.ok() || start > end || end > tables.glyf.size()) {
    return std::nullopt;
  }
  return tables.glyf.subspan(start, end - start);
}

/// Reads \p count point flags, expanding the repeat flag (bit 3).
std::vector<uint8_t> ReadFlags(ByteReader& reader, std::size_t count) {
  std::vector<uint8_t> flags;
  while (flags.size() < count && reader.ok()) {
    const uint8_t flag = reader.u8();
    flags.push_back(flag);
    if (flag & 0x08) {
      for (uint8_t repeat = reader.u8(); repeat > 0 && flags.size() < count; --repeat) {
        flags.push_back(flag);
      }
    }
  }
  return flags;
}

/// Reads one delta-encoded coordinate per flag: a byte whose sign \p sameOrPositiveBit gives when
/// \p shortBit is set, otherwise a signed 16-bit delta unless \p sameOrPositiveBit repeats the
/// previous value.
std::vector<double> ReadCoordinates(ByteReader& reader, const std::vector<uint8_t>& flags,
                                    uint8_t shortBit, uint8_t sameOrPositiveBit) {
  std::vector<double> values;
  double value = 0.0;
  for (const uint8_t flag : flags) {
    if (flag & shortBit) {
      const double delta = reader.u8();
      value += (flag & sameOrPositiveBit) ? delta : -delta;
    } else if (!(flag & sameOrPositiveBit)) {
      value += reader.s16();
    }
    values.push_back(value);
  }
  return values;
}

/// Splits the glyph's points into contours at the last point index of each, or returns nullopt
/// when the indices do not increase within the \p flags.size() points.
std::optional<std::vector<Contour>> SplitContours(const std::vector<std::size_t>& ends,
                                                  const std::vector<uint8_t>& flags,
                                                  const std::vector<double>& xs,
                                                  const std::vector<double>& ys) {
  std::vector<Contour> contours;
  std::size_t start = 0;
  for (const std::size_t end : ends) {
    if (end < start || end >= flags.size()) {
      return std::nullopt;
    }
    Contour& contour = contours.emplace_back();
    for (std::size_t i = start; i <= end; ++i) {
      contour.push_back({xs[i], ys[i], (flags[i] & 0x01) != 0});
    }
    start = end + 1;
  }
  return contours;
}

/// The contours of a simple glyph, or nullopt for a composite or malformed one.
std::optional<std::vector<Contour>> ReadContours(std::span<const uint8_t> glyph) {
  if (glyph.empty()) {
    return std::vector<Contour>{};
  }
  ByteReader reader(glyph);
  const int16_t contourCount = reader.s16();
  if (contourCount < 0) {
    return std::nullopt;
  }
  reader.skip(8);  // Bounding box.
  std::vector<std::size_t> ends;
  for (int16_t i = 0; i < contourCount; ++i) {
    ends.push_back(reader.u16());
  }
  reader.skip(reader.u16());  // Hinting instructions.
  const std::size_t pointCount = ends.empty() ? 0 : ends.back() + 1;
  const std::vector<uint8_t> flags = ReadFlags(reader, pointCount);
  const std::vector<double> xs = ReadCoordinates(reader, flags, 0x02, 0x10);
  const std::vector<double> ys = ReadCoordinates(reader, flags, 0x04, 0x20);
  if (!reader.ok() || flags.size() != pointCount) {
    return std::nullopt;
  }
  return SplitContours(ends, flags, xs, ys);
}

/// \p contour with the on-curve point TrueType implies midway between consecutive off-curve
/// points made explicit, so every off-curve point lies between two on-curve points.
Contour WithImpliedPoints(const Contour& contour) {
  Contour points;
  for (std::size_t i = 0; i < contour.size(); ++i) {
    const OutlinePoint& point = contour[i];
    const OutlinePoint& next = contour[(i + 1) % contour.size()];
    points.push_back(point);
    if (!point.onCurve && !next.onCurve) {
      points.push_back({(point.x + next.x) / 2.0, (point.y + next.y) / 2.0, true});
    }
  }
  return points;
}

/// Twice the signed area that the segment from \p from to \p to contributes to its contour.
double TwiceLineArea(const OutlinePoint& from, const OutlinePoint& to) {
  return from.x * to.y - to.x * from.y;
}

/// Twice the signed area that a quadratic Bezier contributes to its contour: its chord's share
/// plus the region between the chord and the curve, which is exactly 2/3 of the control triangle.
double TwiceQuadraticArea(const OutlinePoint& from, const OutlinePoint& control,
                          const OutlinePoint& to) {
  const double twiceTriangle =
      (control.x - from.x) * (to.y - from.y) - (control.y - from.y) * (to.x - from.x);
  return TwiceLineArea(from, to) + 2.0 / 3.0 * twiceTriangle;
}

/// Twice the signed area enclosed by one TrueType contour.
double TwiceContourArea(const Contour& contour) {
  const Contour points = WithImpliedPoints(contour);
  double twiceArea = 0.0;
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (!points[i].onCurve) {
      continue;
    }
    const OutlinePoint& next = points[(i + 1) % points.size()];
    twiceArea += next.onCurve
                     ? TwiceLineArea(points[i], next)
                     : TwiceQuadraticArea(points[i], next, points[(i + 2) % points.size()]);
  }
  return twiceArea;
}

/// Exact area enclosed by a glyph's contours, in square font units, or nullopt when the glyph
/// cannot be read. Noto Sans contours do not overlap, so the nonzero-filled area is the magnitude
/// of the summed signed contour areas.
std::optional<double> GlyphOutlineArea(const GlyphTables& tables, int glyph) {
  const std::optional<std::span<const uint8_t>> data = GlyphData(tables, glyph);
  const std::optional<std::vector<Contour>> contours =
      data ? ReadContours(*data) : std::optional<std::vector<Contour>>();
  if (!contours) {
    return std::nullopt;
  }
  double twiceArea = 0.0;
  for (const Contour& contour : *contours) {
    twiceArea += TwiceContourArea(contour);
  }
  return twiceArea < 0.0 ? -twiceArea / 2.0 : twiceArea / 2.0;
}

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>());
}

/// Coverage (alpha / 255) summed over every pixel, or over the outermost rows and columns only.
double SummedCoverage(const RendererBitmap& bitmap, bool borderOnly) {
  double coverage = 0.0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    const uint8_t* row = bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes;
    const bool borderRow = y == 0 || y + 1 == bitmap.dimensions.y;
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      if (!borderOnly || borderRow || x == 0 || x + 1 == bitmap.dimensions.x) {
        coverage += row[x * 4 + 3] / 255.0;
      }
    }
  }
  return coverage;
}

/// Exact area enclosed by the outlines of \p text's glyphs at \ref kFontSizePx, in square
/// pixels, or nullopt when a glyph is missing or its outline cannot be read. Glyphs are found
/// through the font's `cmap` with stb_truetype; their outlines are read from `glyf` directly,
/// because stb_truetype rounds the on-curve points TrueType implies to whole font units.
std::optional<double> ExactTextArea(std::span<const uint8_t> fontData, std::string_view text) {
  const std::optional<GlyphTables> tables = FindGlyphTables(fontData);
  stbtt_fontinfo font{};
  if (!tables || stbtt_InitFont(&font, fontData.data(), 0) == 0) {
    return std::nullopt;
  }
  double areaUnits = 0.0;
  for (const char ch : text) {
    const int glyph = stbtt_FindGlyphIndex(&font, ch);
    const std::optional<double> area =
        glyph != 0 ? GlyphOutlineArea(*tables, glyph) : std::optional<double>();
    if (!area) {
      return std::nullopt;
    }
    areaUnits += *area;
  }
  ByteReader head(tables->head, 18);
  const uint16_t unitsPerEm = head.u16();
  if (!head.ok() || unitsPerEm == 0) {
    return std::nullopt;
  }
  const double pxPerUnit = kFontSizePx / unitsPerEm;
  return areaUnits * pxPerUnit * pxPerUnit;
}

// Opaque glyphs cover each pixel by the fraction of its area inside the outline, so the summed
// coverage equals the outline's area. A renderer whose glyph edges sit outside or inside the
// outline changes the sum by about the offset times the outline's perimeter.
TEST(GlyphInkAreaTest, RenderedInkEqualsExactOutlineArea) {
  const std::vector<uint8_t> fontData =
      ReadFile(Runfiles::instance().Rlocation(std::string(kFontFile)));
  const std::optional<double> exactArea = ExactTextArea(fontData, kText);
  ASSERT_THAT(exactArea, Optional(Gt(0.0))) << "outlines of \"" << kText << "\" in " << kFontFile;

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(kScene, warnings);
  ASSERT_FALSE(parsed.hasError()) << parsed.error();
  SVGDocument document = std::move(parsed.result());
  RegisterFontsFromDirectoryForTesting(document,
                                       Runfiles::instance().Rlocation(std::string(kFontDirectory)));

  const RendererBitmap bitmap = RenderDocumentWithActiveBackend(document);
  ASSERT_FALSE(bitmap.empty()) << ActiveRendererBackendName() << " returned an empty snapshot";
  EXPECT_THAT(SummedCoverage(bitmap, /*borderOnly=*/true), DoubleEq(0.0))
      << "the canvas clips the text";

  const double ink = SummedCoverage(bitmap, /*borderOnly=*/false);
  EXPECT_THAT(ink, DoubleNear(*exactArea, *exactArea * kInkRelativeTolerance))
      << ActiveRendererBackendName() << " draws \"" << kText << "\" with " << ink
      << " px^2 of ink; its outline encloses " << *exactArea << " px^2 ("
      << 100.0 * (ink - *exactArea) / *exactArea << "% difference)";
}

}  // namespace
}  // namespace donner::svg
