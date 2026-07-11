#include "donner/svg/text/TextBackendSimple.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <vector>

namespace donner::svg {
namespace {

using ::testing::AllOf;
using ::testing::DoubleEq;
using ::testing::DoubleNear;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::FloatEq;
using ::testing::IsEmpty;

auto GlyphIndexIs(auto matcher) {
  return Field("glyphIndex", &TextBackend::ShapedGlyph::glyphIndex, matcher);
}

auto GlyphXAdvanceIs(auto matcher) {
  return Field("xAdvance", &TextBackend::ShapedGlyph::xAdvance, matcher);
}

auto GlyphYAdvanceIs(auto matcher) {
  return Field("yAdvance", &TextBackend::ShapedGlyph::yAdvance, matcher);
}

auto GlyphXKernIs(auto matcher) {
  return Field("xKern", &TextBackend::ShapedGlyph::xKern, matcher);
}

auto GlyphYKernIs(auto matcher) {
  return Field("yKern", &TextBackend::ShapedGlyph::yKern, matcher);
}

auto GlyphFontSizeScaleIs(auto matcher) {
  return Field("fontSizeScale", &TextBackend::ShapedGlyph::fontSizeScale, matcher);
}

// -- Synthetic sfnt builder ---------------------------------------------------
// Builds a minimal TrueType font that stb_truetype accepts, with hand-authored
// metric tables so every table-derived value asserted below is deterministic.

void AppendBE16(std::vector<uint8_t>* out, uint16_t value) {
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>(value & 0xFF));
}

void AppendBE32(std::vector<uint8_t>* out, uint32_t value) {
  out->push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
  out->push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>(value & 0xFF));
}

void WriteBE16(std::vector<uint8_t>* out, size_t offset, uint16_t value) {
  (*out)[offset + 0] = static_cast<uint8_t>((value >> 8) & 0xFF);
  (*out)[offset + 1] = static_cast<uint8_t>(value & 0xFF);
}

struct TableSpec {
  std::string_view tag;
  std::vector<uint8_t> data;
};

/// Assemble an sfnt binary with sequentially packed, 4-byte-aligned tables.
std::vector<uint8_t> MakeSfnt(const std::vector<TableSpec>& tables) {
  std::vector<uint8_t> data;
  AppendBE32(&data, 0x00010000);
  AppendBE16(&data, static_cast<uint16_t>(tables.size()));
  AppendBE16(&data, 0);
  AppendBE16(&data, 0);
  AppendBE16(&data, 0);

  size_t offset = 12 + tables.size() * 16;
  for (const TableSpec& table : tables) {
    EXPECT_EQ(table.tag.size(), 4u);
    data.push_back(static_cast<uint8_t>(table.tag[0]));
    data.push_back(static_cast<uint8_t>(table.tag[1]));
    data.push_back(static_cast<uint8_t>(table.tag[2]));
    data.push_back(static_cast<uint8_t>(table.tag[3]));
    AppendBE32(&data, 0);  // Checksum (unchecked by stb_truetype).
    AppendBE32(&data, static_cast<uint32_t>(offset));
    AppendBE32(&data, static_cast<uint32_t>(table.data.size()));
    offset += (table.data.size() + 3) & ~size_t{3};
  }

  for (const TableSpec& table : tables) {
    data.insert(data.end(), table.data.begin(), table.data.end());
    while (data.size() % 4 != 0) {
      data.push_back(0);
    }
  }
  return data;
}

std::vector<uint8_t> HeadTable(uint16_t unitsPerEm) {
  std::vector<uint8_t> table(54, 0);
  WriteBE16(&table, 18, unitsPerEm);
  // indexToLocFormat at offset 50 stays 0 (short loca offsets).
  return table;
}

std::vector<uint8_t> HheaTable(int16_t ascent, int16_t descent, int16_t lineGap,
                               uint16_t numHMetrics) {
  std::vector<uint8_t> table(36, 0);
  WriteBE16(&table, 4, static_cast<uint16_t>(ascent));
  WriteBE16(&table, 6, static_cast<uint16_t>(descent));
  WriteBE16(&table, 8, static_cast<uint16_t>(lineGap));
  WriteBE16(&table, 34, numHMetrics);
  return table;
}

std::vector<uint8_t> MaxpTable(uint16_t numGlyphs) {
  std::vector<uint8_t> table;
  AppendBE32(&table, 0x00005000);
  AppendBE16(&table, numGlyphs);
  return table;
}

std::vector<uint8_t> HmtxTable(const std::vector<uint16_t>& advances) {
  std::vector<uint8_t> table;
  for (const uint16_t advance : advances) {
    AppendBE16(&table, advance);
    AppendBE16(&table, 0);  // Left side bearing.
  }
  return table;
}

/// Glyph 1's outline: one contour of (0,0) on-curve, (50,100) off-curve, (100,0)
/// on-curve, decoding to a quadratic curve segment. Padded to 24 bytes.
std::vector<uint8_t> QuadGlyphData() {
  std::vector<uint8_t> glyph;
  AppendBE16(&glyph, 1);  // numberOfContours.
  AppendBE16(&glyph, 0);  // xMin.
  AppendBE16(&glyph, 0);  // yMin.
  AppendBE16(&glyph, 100);  // xMax.
  AppendBE16(&glyph, 100);  // yMax.
  AppendBE16(&glyph, 2);  // endPtsOfContours[0]: last point index.
  AppendBE16(&glyph, 0);  // instructionLength.
  // Flags: bit0 on-curve, bit1 x-short, bit2 y-short, bits 4/5 positive deltas.
  glyph.push_back(0x37);  // (0,0) on-curve.
  glyph.push_back(0x36);  // (+50,+100) off-curve control.
  glyph.push_back(0x17);  // (+50,-100) on-curve.
  glyph.insert(glyph.end(), {0, 50, 50});    // x deltas.
  glyph.insert(glyph.end(), {0, 100, 100});  // y deltas (signs from flags).
  while (glyph.size() < 24) {
    glyph.push_back(0);
  }
  return glyph;
}

/// Short-format loca: glyph 0 empty, glyph 1 = the 24-byte quad glyph, glyphs 2-3 empty.
std::vector<uint8_t> LocaTable() {
  std::vector<uint8_t> table;
  for (const uint16_t halfOffset : {0, 0, 12, 12, 12}) {
    AppendBE16(&table, halfOffset);
  }
  return table;
}

/// cmap with a single format-12 subtable of (start, end, startGlyphId) groups.
std::vector<uint8_t> CmapTable(
    const std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>& groups) {
  std::vector<uint8_t> table;
  AppendBE16(&table, 0);   // Version.
  AppendBE16(&table, 1);   // Number of encoding records.
  AppendBE16(&table, 3);   // Platform: Microsoft.
  AppendBE16(&table, 10);  // Encoding: UCS-4.
  AppendBE32(&table, 12);  // Subtable offset.
  AppendBE16(&table, 12);  // Format 12.
  AppendBE16(&table, 0);   // Reserved.
  AppendBE32(&table, static_cast<uint32_t>(16 + groups.size() * 12));  // Length.
  AppendBE32(&table, 0);  // Language.
  AppendBE32(&table, static_cast<uint32_t>(groups.size()));
  for (const auto& [start, end, startGlyph] : groups) {
    AppendBE32(&table, start);
    AppendBE32(&table, end);
    AppendBE32(&table, startGlyph);
  }
  return table;
}

/// Format-0 kern table with (left glyph, right glyph, value) pairs sorted by key.
std::vector<uint8_t> KernTable(const std::vector<std::tuple<uint16_t, uint16_t, int16_t>>& pairs) {
  std::vector<uint8_t> table;
  AppendBE16(&table, 0);  // Table version.
  AppendBE16(&table, 1);  // Number of subtables.
  AppendBE16(&table, 0);  // Subtable version.
  AppendBE16(&table, static_cast<uint16_t>(14 + pairs.size() * 6));  // Subtable length.
  AppendBE16(&table, 1);  // Coverage: horizontal kerning.
  AppendBE16(&table, static_cast<uint16_t>(pairs.size()));
  AppendBE16(&table, 0);  // searchRange (unused by stb_truetype).
  AppendBE16(&table, 0);  // entrySelector.
  AppendBE16(&table, 0);  // rangeShift.
  for (const auto& [left, right, value] : pairs) {
    AppendBE16(&table, left);
    AppendBE16(&table, right);
    AppendBE16(&table, static_cast<uint16_t>(value));
  }
  return table;
}

std::vector<uint8_t> Os2Table(uint16_t version) {
  std::vector<uint8_t> table(90, 0);
  WriteBE16(&table, 0, version);
  WriteBE16(&table, 16, 111);  // ySubscriptYOffset.
  WriteBE16(&table, 24, 222);  // ySuperscriptYOffset.
  WriteBE16(&table, 26, 33);   // yStrikeoutSize.
  WriteBE16(&table, 28, 250);  // yStrikeoutPosition.
  WriteBE16(&table, 86, 480);  // sxHeight (version >= 2 only).
  return table;
}

std::vector<uint8_t> PostTable() {
  std::vector<uint8_t> table(16, 0);
  WriteBE16(&table, 8, static_cast<uint16_t>(int16_t{-120}));  // underlinePosition.
  WriteBE16(&table, 10, 60);                                   // underlineThickness.
  return table;
}

/// A complete minimal TrueType font: 1000 units/em, glyphs 1='A'/'a', 2='V',
/// 3=U+4E00, with kern pairs (A,V)=-100 and (A,U+4E00)=-80.
std::vector<uint8_t> MakeTestFontData(bool withKern, int os2Version, bool withPost) {
  std::vector<TableSpec> tables = {
      {"cmap", CmapTable({{'A', 'A', 1}, {'V', 'V', 2}, {'a', 'a', 1}, {0x4E00, 0x4E00, 3}})},
      {"glyf", QuadGlyphData()},
      {"head", HeadTable(1000)},
      {"hhea", HheaTable(800, -200, 100, 4)},
      {"hmtx", HmtxTable({400, 500, 600, 700})},
      {"loca", LocaTable()},
      {"maxp", MaxpTable(4)},
  };
  if (withKern) {
    tables.push_back({"kern", KernTable({{1, 2, -100}, {1, 3, -80}})});
  }
  if (os2Version >= 0) {
    tables.push_back({"OS/2", Os2Table(static_cast<uint16_t>(os2Version))});
  }
  if (withPost) {
    tables.push_back({"post", PostTable()});
  }
  return MakeSfnt(tables);
}

class TextBackendSimpleTest : public testing::Test {
protected:
  FontHandle loadFont(const std::vector<uint8_t>& data) {
    const FontHandle font = fontManager_.loadFontData(data);
    EXPECT_TRUE(static_cast<bool>(font));
    return font;
  }

  FontHandle fullFont() { return loadFont(MakeTestFontData(true, 2, true)); }

  Registry registry_;
  FontManager fontManager_{registry_};
  TextBackendSimple backend_{fontManager_, registry_};
};

// -- Invalid and degenerate fonts ---------------------------------------------

TEST_F(TextBackendSimpleTest, NullFontHandleYieldsEmptyResults) {
  const FontHandle nullFont;

  const FontVMetrics metrics = backend_.fontVMetrics(nullFont);
  EXPECT_EQ(metrics.ascent, 0);
  EXPECT_EQ(metrics.descent, 0);
  EXPECT_EQ(metrics.xHeight, 0);

  EXPECT_FLOAT_EQ(backend_.scaleForPixelHeight(nullFont, 16.0f), 0.0f);
  EXPECT_FLOAT_EQ(backend_.scaleForEmToPixels(nullFont, 16.0f), 0.0f);
  EXPECT_FALSE(backend_.underlineMetrics(nullFont).has_value());
  EXPECT_FALSE(backend_.strikeoutMetrics(nullFont).has_value());
  EXPECT_FALSE(backend_.subSuperMetrics(nullFont).has_value());
  EXPECT_TRUE(backend_.glyphOutline(nullFont, 1, 1.0f).empty());
  EXPECT_FALSE(backend_.isBitmapOnly(nullFont));
  EXPECT_THAT(backend_.shapeRun(nullFont, 16.0f, "AV", 0, 2, false, FontVariant::Normal, false)
                  .glyphs,
              IsEmpty());
  EXPECT_DOUBLE_EQ(backend_.crossSpanKern(nullFont, 16.0f, nullFont, 16.0f, 'A', 'V', false), 0.0);
}

TEST_F(TextBackendSimpleTest, NonOutlineFontFallsBackToHeadUnitsPerEm) {
  // Only a head table: no glyf/CFF outlines, so stb parsing is skipped entirely.
  const FontHandle font = loadFont(MakeSfnt({{"head", HeadTable(2048)}}));

  EXPECT_TRUE(backend_.isBitmapOnly(font));
  EXPECT_FLOAT_EQ(backend_.scaleForPixelHeight(font, 512.0f), 512.0f / 2048.0f);
  EXPECT_FLOAT_EQ(backend_.scaleForEmToPixels(font, 512.0f), 0.0f);
  EXPECT_EQ(backend_.fontVMetrics(font).ascent, 0);
  EXPECT_THAT(backend_.shapeRun(font, 16.0f, "A", 0, 1, false, FontVariant::Normal, false).glyphs,
              IsEmpty());
}

TEST_F(TextBackendSimpleTest, UnparseableOutlineFontFailsInitOnceAndCachesFailure) {
  // A glyf tag makes HasOutlineTables() pass, but without cmap stbtt_InitFont fails.
  const FontHandle font = loadFont(MakeSfnt({{"glyf", {0, 0, 0, 0}}, {"head", HeadTable(1000)}}));

  EXPECT_EQ(backend_.fontVMetrics(font).ascent, 0);
  // Second call hits the cached invalid parse state.
  EXPECT_EQ(backend_.fontVMetrics(font).ascent, 0);
  EXPECT_TRUE(backend_.glyphOutline(font, 1, 1.0f).empty());
  EXPECT_FLOAT_EQ(backend_.scaleForPixelHeight(font, 100.0f), 0.1f);
}

// -- Table-derived metrics ----------------------------------------------------

TEST_F(TextBackendSimpleTest, MetricsComeFromAuthoredTables) {
  const FontHandle font = fullFont();

  const FontVMetrics metrics = backend_.fontVMetrics(font);
  EXPECT_EQ(metrics.ascent, 800);
  EXPECT_EQ(metrics.descent, -200);
  EXPECT_EQ(metrics.lineGap, 100);
  EXPECT_EQ(metrics.xHeight, 480);

  const auto underline = backend_.underlineMetrics(font);
  ASSERT_TRUE(underline.has_value());
  EXPECT_DOUBLE_EQ(underline->position, -120.0);
  EXPECT_DOUBLE_EQ(underline->thickness, 60.0);

  const auto strikeout = backend_.strikeoutMetrics(font);
  ASSERT_TRUE(strikeout.has_value());
  EXPECT_DOUBLE_EQ(strikeout->thickness, 33.0);
  EXPECT_DOUBLE_EQ(strikeout->position, 250.0);

  const auto subSuper = backend_.subSuperMetrics(font);
  ASSERT_TRUE(subSuper.has_value());
  EXPECT_EQ(subSuper->subscriptYOffset, 111);
  EXPECT_EQ(subSuper->superscriptYOffset, 222);

  EXPECT_FLOAT_EQ(backend_.scaleForPixelHeight(font, 100.0f), 0.1f);
  EXPECT_FLOAT_EQ(backend_.scaleForEmToPixels(font, 100.0f), 0.1f);
  EXPECT_FALSE(backend_.isBitmapOnly(font));
}

TEST_F(TextBackendSimpleTest, Os2VersionBelowTwoOmitsXHeight) {
  const FontHandle font = loadFont(MakeTestFontData(false, 1, false));

  const FontVMetrics metrics = backend_.fontVMetrics(font);
  EXPECT_EQ(metrics.ascent, 800);
  EXPECT_EQ(metrics.xHeight, 0);
}

TEST_F(TextBackendSimpleTest, MissingOs2AndPostTablesYieldNullopt) {
  const FontHandle font = loadFont(MakeTestFontData(false, -1, false));

  EXPECT_FALSE(backend_.underlineMetrics(font).has_value());
  EXPECT_FALSE(backend_.strikeoutMetrics(font).has_value());
  EXPECT_FALSE(backend_.subSuperMetrics(font).has_value());
  // Vertical metrics still come from hhea.
  EXPECT_EQ(backend_.fontVMetrics(font).ascent, 800);
}

// -- Shaping ------------------------------------------------------------------

TEST_F(TextBackendSimpleTest, ShapeRunAppliesHorizontalKerning) {
  const FontHandle font = fullFont();

  // 100px at 1000 units/em scales design units by 0.1 (as a float).
  const auto shaped =
      backend_.shapeRun(font, 100.0f, "AV", 0, 2, false, FontVariant::Normal, false);
  EXPECT_THAT(shaped.glyphs,
              ElementsAre(AllOf(GlyphIndexIs(1), GlyphXAdvanceIs(DoubleNear(50.0, 1e-4)),
                                GlyphXKernIs(DoubleEq(0.0))),
                          AllOf(GlyphIndexIs(2), GlyphXAdvanceIs(DoubleNear(60.0, 1e-4)),
                                GlyphXKernIs(DoubleNear(-10.0, 1e-4)),
                                GlyphYKernIs(DoubleEq(0.0)))));
}

TEST_F(TextBackendSimpleTest, ShapeRunVerticalLatinUsesSidewaysAdvancesAndVerticalKern) {
  const FontHandle font = fullFont();

  const auto shaped = backend_.shapeRun(font, 100.0f, "AV", 0, 2, true, FontVariant::Normal, false);
  EXPECT_THAT(shaped.glyphs,
              ElementsAre(AllOf(GlyphXAdvanceIs(DoubleEq(0.0)),
                                GlyphYAdvanceIs(DoubleNear(50.0, 1e-4))),
                          AllOf(GlyphXAdvanceIs(DoubleEq(0.0)),
                                GlyphYAdvanceIs(DoubleNear(60.0, 1e-4)),
                                GlyphYKernIs(DoubleNear(-10.0, 1e-4)),
                                GlyphXKernIs(DoubleEq(0.0)))));
}

TEST_F(TextBackendSimpleTest, ShapeRunVerticalCjkAdvancesByEmHeight) {
  const FontHandle font = fullFont();

  // U+4E00 in vertical mode: upright glyph, advance equals the font size.
  const auto shaped = backend_.shapeRun(font, 100.0f, "\xE4\xB8\x80", 0, 3, true,
                                        FontVariant::Normal, false);
  EXPECT_THAT(shaped.glyphs,
              ElementsAre(AllOf(GlyphIndexIs(3), GlyphXAdvanceIs(DoubleEq(0.0)),
                                GlyphYAdvanceIs(DoubleEq(100.0)))));
}

TEST_F(TextBackendSimpleTest, ShapeRunZeroFontSizeReturnsEmpty) {
  const FontHandle font = fullFont();

  const auto shaped = backend_.shapeRun(font, 0.0f, "AV", 0, 2, false, FontVariant::Normal, false);
  EXPECT_THAT(shaped.glyphs, IsEmpty());
}

TEST_F(TextBackendSimpleTest, SmallCapsSynthesisUppercasesAndScalesLowercase) {
  const FontHandle font = fullFont();

  const auto shaped =
      backend_.shapeRun(font, 100.0f, "aV", 0, 2, false, FontVariant::SmallCaps, false);
  // 'a' maps to the 'A' glyph at 80% scale; kerning still uses the full-size scale.
  EXPECT_THAT(shaped.glyphs,
              ElementsAre(AllOf(GlyphIndexIs(1), GlyphFontSizeScaleIs(FloatEq(0.8f)),
                                GlyphXAdvanceIs(DoubleNear(40.0, 1e-4))),
                          AllOf(GlyphIndexIs(2), GlyphFontSizeScaleIs(FloatEq(1.0f)),
                                GlyphXAdvanceIs(DoubleNear(60.0, 1e-4)),
                                GlyphXKernIs(DoubleNear(-10.0, 1e-4)))));
}

// -- Cross-span kerning -------------------------------------------------------

TEST_F(TextBackendSimpleTest, CrossSpanKernAppliesPerWritingModeAndOrientation) {
  const FontHandle font = fullFont();

  // Horizontal: kern pair (A, V) = -100 units at scale 0.1.
  EXPECT_NEAR(backend_.crossSpanKern(font, 100.0f, font, 100.0f, 'A', 'V', false), -10.0, 1e-4);
  // Vertical sideways Latin still kerns along the advance direction.
  EXPECT_NEAR(backend_.crossSpanKern(font, 100.0f, font, 100.0f, 'A', 'V', true), -10.0, 1e-4);
  // Horizontal kerning against an upright CJK codepoint.
  EXPECT_NEAR(backend_.crossSpanKern(font, 100.0f, font, 100.0f, 'A', 0x4E00, false), -8.0, 1e-4);
  // Vertical upright CJK never kerns, even with a matching kern pair.
  EXPECT_DOUBLE_EQ(backend_.crossSpanKern(font, 100.0f, font, 100.0f, 'A', 0x4E00, true), 0.0);
  // No kern pair defined in the (V, A) direction.
  EXPECT_DOUBLE_EQ(backend_.crossSpanKern(font, 100.0f, font, 100.0f, 'V', 'A', false), 0.0);
}

// -- Glyph outlines -----------------------------------------------------------

TEST_F(TextBackendSimpleTest, GlyphOutlineEmptyForEmptyGlyph) {
  const FontHandle font = fullFont();

  // Glyph 2 ('V') has a zero-length glyf entry.
  EXPECT_TRUE(backend_.glyphOutline(font, 2, 0.1f).empty());
}

TEST_F(TextBackendSimpleTest, GlyphOutlineDecodesQuadraticSegmentsWithFlippedY) {
  const FontHandle font = fullFont();

  // Glyph 1 is a single quadratic contour; Y flips from font-up to SVG-down.
  const Path path = backend_.glyphOutline(font, 1, 0.1f);
  ASSERT_FALSE(path.empty());

  const bool hasQuad =
      std::any_of(path.commands().begin(), path.commands().end(), [](const auto& command) {
        return command.verb == Path::Verb::QuadTo;
      });
  EXPECT_TRUE(hasQuad);

  const Box2d bounds = path.bounds();
  EXPECT_NEAR(bounds.topLeft.x, 0.0, 1e-4);
  EXPECT_NEAR(bounds.bottomRight.x, 10.0, 1e-4);
  EXPECT_LT(bounds.topLeft.y, 0.0);        // Above the baseline in SVG coordinates.
  EXPECT_NEAR(bounds.bottomRight.y, 0.0, 1e-4);
}

TEST_F(TextBackendSimpleTest, GlyphOutlineDecodesCurveSegmentsFromRealFont) {
  const FontHandle font = fontManager_.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  const auto shaped = backend_.shapeRun(font, 100.0f, "O", 0, 1, false, FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, ElementsAre(GlyphIndexIs(testing::Gt(0))));

  const Path path =
      backend_.glyphOutline(font, shaped.glyphs[0].glyphIndex, 0.1f);
  ASSERT_FALSE(path.empty());

  // The letter 'O' must contain curve segments (quadratic for TrueType outlines,
  // cubic for CFF outlines).
  const bool hasCurves =
      std::any_of(path.commands().begin(), path.commands().end(), [](const auto& command) {
        return command.verb == Path::Verb::QuadTo || command.verb == Path::Verb::CurveTo;
      });
  EXPECT_TRUE(hasCurves);
}

// -- Capability contracts -----------------------------------------------------

TEST_F(TextBackendSimpleTest, SimpleBackendHasNoAdvancedCapabilities) {
  const FontHandle font = fullFont();

  EXPECT_FALSE(backend_.isCursive('A'));
  EXPECT_FALSE(backend_.isCursive(0x0627));  // Arabic alef: cursive in the full backend only.
  EXPECT_FALSE(backend_.hasSmallCapsFeature(font));
  EXPECT_FALSE(backend_.bitmapGlyph(font, 1, 1.0f).has_value());
}

}  // namespace
}  // namespace donner::svg
