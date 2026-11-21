#include "donner/svg/text/TextBackend.h"

#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/css/FontFace.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/text/TextBackendFull.h"
#include "donner/svg/text/TextBackendSimple.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg {

namespace {

using ::testing::AllOf;
using ::testing::DoubleEq;
using ::testing::DoubleNear;
using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::FloatEq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::IsEmpty;
using ::testing::Lt;
using ::testing::Not;
using ::testing::Pointwise;
using ::testing::SizeIs;

FontHandle LoadResvgFont(FontManager& fontManager, const std::string& fontFilename,
                         const std::string& familyName, bool trusted = true) {
  const std::string fontsDir = Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts");
  const std::string fontPath = fontsDir + "/" + fontFilename;

  std::ifstream file(fontPath, std::ios::binary);
  if (!file) {
    return {};
  }

  file.seekg(0, std::ios::end);
  const auto size = file.tellg();
  file.seekg(0);

  auto fontData = std::make_shared<const std::vector<uint8_t>>(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(const_cast<uint8_t*>(fontData->data())), size);

  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Data;
  source.payload = fontData;
  source.trusted = trusted;

  css::FontFace face;
  face.familyName = RcString(familyName);
  face.sources.push_back(std::move(source));

  fontManager.addFontFace(face);
  return fontManager.findFont(RcString(familyName));
}

uint16_t ReadBe16(std::span<const uint8_t> data, size_t offset) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
}

uint32_t ReadBe32(std::span<const uint8_t> data, size_t offset) {
  return (static_cast<uint32_t>(data[offset]) << 24) |
         (static_cast<uint32_t>(data[offset + 1]) << 16) |
         (static_cast<uint32_t>(data[offset + 2]) << 8) | data[offset + 3];
}

/// Byte offset of a glyph's advance height in `vmtx`, whose last long metric repeats. Requires
/// \p longMetrics > 0.
size_t VmtxAdvanceOffset(size_t glyph, size_t longMetrics) {
  return std::min(glyph, longMetrics - 1) * 4;
}

/// Byte offset of a glyph's top side bearing in `vmtx`, including the trailing bearing array.
size_t VmtxTopSideBearingOffset(size_t glyph, size_t longMetrics) {
  return glyph < longMetrics ? glyph * 4 + 2 : longMetrics * 4 + (glyph - longMetrics) * 2;
}

void WriteBe16(std::vector<uint8_t>* data, size_t offset, uint16_t value) {
  (*data)[offset] = static_cast<uint8_t>(value >> 8);
  (*data)[offset + 1] = static_cast<uint8_t>(value);
}

void WriteBe32(std::vector<uint8_t>* data, size_t offset, uint32_t value) {
  (*data)[offset] = static_cast<uint8_t>(value >> 24);
  (*data)[offset + 1] = static_cast<uint8_t>(value >> 16);
  (*data)[offset + 2] = static_cast<uint8_t>(value >> 8);
  (*data)[offset + 3] = static_cast<uint8_t>(value);
}

std::vector<uint8_t> ReadResvgFontBytes(const std::string& fontFilename) {
  const std::string fontsDir = Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts");
  std::ifstream file(fontsDir + "/" + fontFilename, std::ios::binary);
  if (!file) {
    return {};
  }
  file.seekg(0, std::ios::end);
  const auto size = file.tellg();
  file.seekg(0);
  std::vector<uint8_t> result(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(result.data()), size);
  return result;
}

std::vector<uint8_t> AddEmptyBitmapTables(std::span<const uint8_t> input) {
  if (input.size() < 12) {
    return {};
  }
  const uint16_t oldTableCount = ReadBe16(input, 4);
  const size_t oldDirectorySize = 12u + static_cast<size_t>(oldTableCount) * 16u;
  if (oldDirectorySize > input.size() || oldTableCount > UINT16_MAX - 2u) {
    return {};
  }

  constexpr size_t kAddedDirectoryBytes = 32;
  const uint16_t newTableCount = static_cast<uint16_t>(oldTableCount + 2u);
  const size_t newDirectorySize = oldDirectorySize + kAddedDirectoryBytes;
  std::vector<uint8_t> result(newDirectorySize + input.size() - oldDirectorySize, 0);
  std::copy_n(input.begin(), 12, result.begin());
  WriteBe16(&result, 4, newTableCount);
  uint16_t powerOfTwo = 1;
  uint16_t entrySelector = 0;
  while (static_cast<uint32_t>(powerOfTwo) * 2u <= newTableCount) {
    powerOfTwo = static_cast<uint16_t>(powerOfTwo * 2u);
    ++entrySelector;
  }
  WriteBe16(&result, 6, static_cast<uint16_t>(powerOfTwo * 16u));
  WriteBe16(&result, 8, entrySelector);
  WriteBe16(&result, 10, static_cast<uint16_t>(newTableCount * 16u - powerOfTwo * 16u));

  for (uint16_t i = 0; i < oldTableCount; ++i) {
    const size_t recordOffset = 12u + static_cast<size_t>(i) * 16u;
    std::copy_n(input.begin() + recordOffset, 16, result.begin() + recordOffset);
    WriteBe32(&result, recordOffset + 8u,
              ReadBe32(input, recordOffset + 8u) + kAddedDirectoryBytes);
  }
  std::copy(input.begin() + oldDirectorySize, input.end(), result.begin() + newDirectorySize);

  const auto appendTable = [&](size_t recordOffset, std::string_view tag,
                               std::span<const uint8_t> bytes) {
    while (result.size() % 4u != 0) {
      result.push_back(0);
    }
    const size_t tableOffset = result.size();
    result.insert(result.end(), bytes.begin(), bytes.end());
    std::copy(tag.begin(), tag.end(), result.begin() + recordOffset);
    WriteBe32(&result, recordOffset + 8u, static_cast<uint32_t>(tableOffset));
    WriteBe32(&result, recordOffset + 12u, static_cast<uint32_t>(bytes.size()));
  };
  constexpr std::array<uint8_t, 4> kCbdtHeader{0, 3, 0, 0};
  constexpr std::array<uint8_t, 8> kCblcHeader{0, 3, 0, 0, 0, 0, 0, 0};
  appendTable(12u + static_cast<size_t>(oldTableCount) * 16u, "CBDT", kCbdtHeader);
  appendTable(12u + static_cast<size_t>(oldTableCount + 1u) * 16u, "CBLC", kCblcHeader);
  return result;
}

std::vector<uint8_t> AddEmptyGlyfTable(std::span<const uint8_t> input) {
  if (input.size() < 12) {
    return {};
  }
  const uint16_t oldTableCount = ReadBe16(input, 4);
  const size_t oldDirectorySize = 12u + static_cast<size_t>(oldTableCount) * 16u;
  if (oldDirectorySize > input.size() || oldTableCount == UINT16_MAX) {
    return {};
  }

  constexpr size_t kAddedDirectoryBytes = 16;
  const uint16_t newTableCount = static_cast<uint16_t>(oldTableCount + 1u);
  const size_t newDirectorySize = oldDirectorySize + kAddedDirectoryBytes;
  std::vector<uint8_t> result(newDirectorySize + input.size() - oldDirectorySize, 0);
  std::copy_n(input.begin(), 12, result.begin());
  WriteBe16(&result, 4, newTableCount);
  for (uint16_t table = 0; table < oldTableCount; ++table) {
    const size_t recordOffset = 12u + static_cast<size_t>(table) * 16u;
    std::copy_n(input.begin() + recordOffset, 16, result.begin() + recordOffset);
    WriteBe32(&result, recordOffset + 8u,
              ReadBe32(input, recordOffset + 8u) + kAddedDirectoryBytes);
  }
  std::copy(input.begin() + oldDirectorySize, input.end(), result.begin() + newDirectorySize);

  const size_t glyfRecord = oldDirectorySize;
  constexpr std::string_view kGlyf = "glyf";
  std::copy(kGlyf.begin(), kGlyf.end(), result.begin() + glyfRecord);
  WriteBe32(&result, glyfRecord + 8u, static_cast<uint32_t>(result.size()));
  return result;
}

auto GlyphIndexIs(auto matcher) {
  return Field("glyphIndex", &TextBackend::ShapedGlyph::glyphIndex, matcher);
}

auto GlyphXAdvanceIs(auto matcher) {
  return Field("xAdvance", &TextBackend::ShapedGlyph::xAdvance, matcher);
}

auto GlyphYAdvanceIs(auto matcher) {
  return Field("yAdvance", &TextBackend::ShapedGlyph::yAdvance, matcher);
}

auto GlyphXOffsetIs(auto matcher) {
  return Field("xOffset", &TextBackend::ShapedGlyph::xOffset, matcher);
}

auto GlyphYOffsetIs(auto matcher) {
  return Field("yOffset", &TextBackend::ShapedGlyph::yOffset, matcher);
}

auto GlyphClusterIs(auto matcher) {
  return Field("cluster", &TextBackend::ShapedGlyph::cluster, matcher);
}

auto GlyphFontSizeScaleIs(auto matcher) {
  return Field("fontSizeScale", &TextBackend::ShapedGlyph::fontSizeScale, matcher);
}

MATCHER_P(AdvanceNear, tolerance, "") {
  const auto& actual = std::get<0>(arg);
  const auto& expected = std::get<1>(arg);
  return testing::ExplainMatchResult(DoubleNear(expected.xAdvance, tolerance), actual.xAdvance,
                                     result_listener);
}

MATCHER_P(GlyphPairHasSameIndexAndAdvanceNear, tolerance,
          "has two glyphs with equal glyph index and near-equal xAdvance") {
  if (arg.size() != 2) {
    *result_listener << "has " << arg.size() << " glyphs";
    return false;
  }

  const auto& first = arg[0];
  const auto& second = arg[1];
  bool matches = true;

  if (first.glyphIndex != second.glyphIndex) {
    *result_listener << "glyph indices are " << first.glyphIndex << " and " << second.glyphIndex;
    matches = false;
  }

  const double advanceDifference = std::abs(first.xAdvance - second.xAdvance);
  if (advanceDifference > tolerance) {
    if (!matches) {
      *result_listener << "; ";
    }

    *result_listener << "xAdvance values are " << first.xAdvance << " and " << second.xAdvance
                     << ", difference " << advanceDifference << " exceeds " << tolerance;
    matches = false;
  }

  return matches;
}

MATCHER(HasNondecreasingClusters, "has nondecreasing glyph clusters") {
  if (arg.empty()) {
    return true;
  }

  uint32_t previousCluster = arg[0].cluster;
  for (size_t index = 1; index < arg.size(); ++index) {
    const uint32_t cluster = arg[index].cluster;
    if (cluster < previousCluster) {
      *result_listener << "glyph " << index << " has cluster " << cluster
                       << " after previous cluster " << previousCluster;
      return false;
    }

    previousCluster = cluster;
  }

  return true;
}

MATCHER(HasPositiveAdvanceInEitherAxis, "has positive xAdvance + yAdvance") {
  if (arg.xAdvance + arg.yAdvance > 0.0) {
    return true;
  }

  *result_listener << "xAdvance=" << arg.xAdvance << ", yAdvance=" << arg.yAdvance;
  return false;
}

// ── Backend selection ───────────────────────────────────────────────────────

enum class BackendType { Simple, Full };

std::string BackendName(const testing::TestParamInfo<BackendType>& info) {
  return info.param == BackendType::Simple ? "Simple" : "Full";
}

class TextBackendTest : public testing::TestWithParam<BackendType> {
protected:
  void SetUp() override {
    if (GetParam() == BackendType::Simple) {
      backend_ = std::make_unique<TextBackendSimple>(fontManager_, registry_);
    } else {
      backend_ = std::make_unique<TextBackendFull>(fontManager_, registry_);
    }
  }

  bool isSimple() const { return GetParam() == BackendType::Simple; }
  bool isFull() const { return GetParam() == BackendType::Full; }

  TextBackend& backend() { return *backend_; }

  FontHandle fallbackFont() { return fontManager_.fallbackFont(); }

  FontHandle loadFont(const std::string& filename, const std::string& family) {
    return LoadResvgFont(fontManager_, filename, family);
  }

  Registry registry_;
  FontManager fontManager_{registry_};
  std::unique_ptr<TextBackend> backend_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Shared contract tests - both backends must satisfy these.
// ═══════════════════════════════════════════════════════════════════════════

// ── Font metrics ────────────────────────────────────────────────────────────

TEST_P(TextBackendTest, FontVMetricsReturnsNonZeroValues) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  const FontVMetrics metrics = backend().fontVMetrics(font);
  EXPECT_GT(metrics.ascent, 0);
  EXPECT_LT(metrics.descent, 0);
}

TEST_P(TextBackendTest, MissingXHeightUsesTheSelectedGlyphMetric) {
  std::vector<uint8_t> bytes = ReadResvgFontBytes("NotoSans-Regular.ttf");
  ASSERT_GE(bytes.size(), 12u);
  bool changed = false;
  for (size_t i = 0; i < ReadBe16(bytes, 4); ++i) {
    const size_t entry = 12 + i * 16;
    ASSERT_LE(entry + 16, bytes.size());
    if (ReadBe32(bytes, entry) == 0x4F532F32) {
      const size_t offset = ReadBe32(bytes, entry + 8);
      ASSERT_LE(offset + 88, bytes.size());
      WriteBe16(&bytes, offset, 1);
      WriteBe16(&bytes, offset + 86, 0);
      changed = true;
      break;
    }
  }
  ASSERT_THAT(changed, testing::IsTrue());
  const FontDataTrust trust = isSimple() ? FontDataTrust::Trusted : FontDataTrust::Untrusted;
  const FontHandle font = fontManager_.loadFontData(bytes, trust);
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  const auto shaped = backend().shapeRun(font, 20.0f, "x", 0, 1, false, FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, SizeIs(1));
  const Path outline = backend().glyphOutline(font, shaped.glyphs.front().glyphIndex, 1.0f);
  ASSERT_THAT(outline.empty(), testing::IsFalse());
  const double actualXHeight = -outline.bounds().topLeft.y;
  ASSERT_GT(actualXHeight, 0.0);
  EXPECT_DOUBLE_EQ(backend().fontVMetrics(font).xHeight, actualXHeight);
  EXPECT_EQ(backend().fontVMetrics(font).unitsPerEm, 1000);
}

TEST_P(TextBackendTest, CrossSpanKerningPreservesActualFamilyAcrossFacesAndSizes) {
  const FontHandle regular = loadFont("NotoSans-Regular.ttf", "First Alias");
  const FontHandle bold = loadFont("NotoSans-Bold.ttf", "Different Alias");
  ASSERT_THAT(static_cast<bool>(regular), testing::IsTrue());
  ASSERT_THAT(static_cast<bool>(bold), testing::IsTrue());
  ASSERT_NE(regular, bold);
  const double expected = backend().crossSpanKern(regular, 20.0f, regular, 20.0f, 'A', 'V', false);
  ASSERT_LT(expected, 0.0);
  EXPECT_DOUBLE_EQ(backend().crossSpanKern(regular, 20.0f, regular, 30.0f, 'A', 'V', false),
                   expected);
  EXPECT_DOUBLE_EQ(backend().crossSpanKern(regular, 20.0f, bold, 20.0f, 'A', 'V', false), expected);
  EXPECT_DOUBLE_EQ(backend().crossSpanKern(regular, 20.0f, bold, 30.0f, 'A', 'V', false), expected);
}

TEST_P(TextBackendTest, CrossSpanKerningRejectsUnrelatedFacesAndInvalidSizes) {
  const FontHandle first = loadFont("NotoSans-Regular.ttf", "Shared Alias");
  const FontHandle other = loadFont("MPLUS1p-Regular.ttf", "Shared Alias");
  ASSERT_THAT(static_cast<bool>(first), testing::IsTrue());
  ASSERT_THAT(static_cast<bool>(other), testing::IsTrue());
  ASSERT_NE(first, other);
  ASSERT_LT(backend().crossSpanKern(first, 20.0f, first, 20.0f, 'A', 'V', false), 0.0);
  EXPECT_DOUBLE_EQ(backend().crossSpanKern(first, 20.0f, other, 20.0f, 'A', 'V', false), 0.0);
  for (float size : {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
                     std::numeric_limits<float>::quiet_NaN()}) {
    SCOPED_TRACE(size);
    EXPECT_DOUBLE_EQ(backend().crossSpanKern(first, size, first, 20.0f, 'A', 'V', false), 0.0);
    EXPECT_DOUBLE_EQ(backend().crossSpanKern(first, 20.0f, first, size, 'A', 'V', false), 0.0);
  }
}

TEST_P(TextBackendTest, FontVMetricsReturnsZeroForInvalidFont) {
  const FontVMetrics metrics = backend().fontVMetrics(FontHandle{});
  EXPECT_EQ(metrics.ascent, 0);
  EXPECT_EQ(metrics.descent, 0);
  EXPECT_EQ(metrics.lineGap, 0);
  EXPECT_EQ(metrics.unitsPerEm, 0);
}

TEST_P(TextBackendTest, ScaleForPixelHeightIsPositive) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  EXPECT_GT(backend().scaleForPixelHeight(font, 16.0f), 0.0f);
}

TEST_P(TextBackendTest, ScaleForEmToPixelsIsPositive) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  EXPECT_GT(backend().scaleForEmToPixels(font, 16.0f), 0.0f);
}

// ── Table-derived metrics ───────────────────────────────────────────────────

TEST_P(TextBackendTest, UnderlineMetricsPresent) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  const auto underline = backend().underlineMetrics(font);
  ASSERT_TRUE(underline.has_value());
  EXPECT_NE(underline->position, 0.0);
  EXPECT_GT(underline->thickness, 0.0);
}

TEST_P(TextBackendTest, UnderlineMetricsNulloptForInvalidFont) {
  EXPECT_FALSE(backend().underlineMetrics(FontHandle{}).has_value());
}

TEST_P(TextBackendTest, StrikeoutMetricsMatchTheOS2TableAndRejectInvalidFonts) {
  const FontHandle font = loadFont("NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(font));
  const auto os2 = fontManager_.sfntTable(font, "OS/2");
  ASSERT_TRUE(os2.has_value());
  ASSERT_GE(os2->size(), 30u);
  const int16_t expectedThickness = static_cast<int16_t>(ReadBe16(*os2, 26));
  const int16_t expectedPosition = static_cast<int16_t>(ReadBe16(*os2, 28));
  ASSERT_GT(expectedThickness, 0);
  ASSERT_GT(expectedPosition, 0);

  const auto strikeout = backend().strikeoutMetrics(font);
  ASSERT_TRUE(strikeout.has_value());
  EXPECT_DOUBLE_EQ(strikeout->thickness, expectedThickness);
  EXPECT_DOUBLE_EQ(strikeout->position, expectedPosition);
  EXPECT_FALSE(backend().strikeoutMetrics(FontHandle{}).has_value());
}

TEST_P(TextBackendTest, SubSuperMetricsPresent) {
  const FontHandle font = loadFont("NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(font));

  const auto subSuper = backend().subSuperMetrics(font);
  ASSERT_TRUE(subSuper.has_value());
  EXPECT_GT(subSuper->subscriptYOffset, 0);
  EXPECT_GT(subSuper->superscriptYOffset, 0);
}

// ── Glyph outlines ──────────────────────────────────────────────────────────

TEST_P(TextBackendTest, GlyphOutlineProducesNonEmptyPath) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  const auto shaped = backend().shapeRun(font, 16.0f, "A", 0, 1, false, FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, ElementsAre(GlyphIndexIs(Gt(0))));

  const float scale = backend().scaleForEmToPixels(font, 16.0f);
  const Path path = backend().glyphOutline(font, shaped.glyphs[0].glyphIndex, scale);
  EXPECT_FALSE(path.empty());
  EXPECT_THAT(path.commands(), Not(IsEmpty()));
}

TEST_P(TextBackendTest, GlyphOutlineEmptyForInvalidFont) {
  const Path path = backend().glyphOutline(FontHandle{}, 1, 1.0f);
  EXPECT_THAT(path.commands(), IsEmpty());
  EXPECT_THAT(path.points(), IsEmpty());
}

TEST_P(TextBackendTest, IsBitmapOnlyFalseForOutlineFont) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  EXPECT_FALSE(backend().isBitmapOnly(font));
}

// ── Shaping ─────────────────────────────────────────────────────────────────

TEST_P(TextBackendTest, NonfiniteAndNegativeSizesAreRejectedBeforeShaping) {
  const FontHandle font = fallbackFont();
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  for (float size :
       {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), -1.0f}) {
    SCOPED_TRACE(size);
    for (FontVariant variant : {FontVariant::Normal, FontVariant::SmallCaps}) {
      EXPECT_THAT(backend().shapeRun(font, size, "aV", 0, 2, false, variant, false).glyphs,
                  IsEmpty());
      EXPECT_THAT(backend().shapeRunNoKerning(font, size, "aV", 0, 2, false, variant, false).glyphs,
                  IsEmpty());
    }
    EXPECT_DOUBLE_EQ(backend().crossSpanKern(font, size, font, size, 'A', 'V', false), 0.0);
  }
}

TEST(TextBackendFullCapabilities, UnrepresentableFixedPointSizesAreRejected) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull backend(fontManager, registry);
  const FontHandle font = fontManager.fallbackFont();
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  for (float size : {1e20f, std::numeric_limits<float>::max()}) {
    SCOPED_TRACE(size);
    for (FontVariant variant : {FontVariant::Normal, FontVariant::SmallCaps}) {
      EXPECT_THAT(backend.shapeRun(font, size, "aV", 0, 2, false, variant, false).glyphs,
                  IsEmpty());
      EXPECT_THAT(backend.shapeRunNoKerning(font, size, "aV", 0, 2, false, variant, false).glyphs,
                  IsEmpty());
    }
    EXPECT_DOUBLE_EQ(backend.crossSpanKern(font, size, font, size, 'A', 'V', false), 0.0);
  }
}

TEST_P(TextBackendTest, ShapeRunProducesGlyphsForLatinText) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  const auto shaped =
      backend().shapeRun(font, 16.0f, "Hello", 0, 5, false, FontVariant::Normal, false);
  EXPECT_THAT(shaped.glyphs, AllOf(SizeIs(5), HasNondecreasingClusters(),
                                   Each(AllOf(GlyphIndexIs(Gt(0)), GlyphXAdvanceIs(Gt(0.0)),
                                              GlyphFontSizeScaleIs(FloatEq(1.0f))))));
}

TEST_P(TextBackendTest, ShapeRunHandlesSubstring) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  // Shape only "ll" from "Hello" (bytes 2-4).
  const auto shaped =
      backend().shapeRun(font, 16.0f, "Hello", 2, 2, false, FontVariant::Normal, false);
  // Both glyphs are 'l' with near-equal advances; HarfBuzz GPOS may adjust slightly per context.
  EXPECT_THAT(shaped.glyphs, GlyphPairHasSameIndexAndAdvanceNear(1.0));
}

TEST_P(TextBackendTest, ShapeRunEmptyForInvalidFont) {
  const auto shaped =
      backend().shapeRun(FontHandle{}, 16.0f, "Hello", 0, 5, false, FontVariant::Normal, false);
  EXPECT_THAT(shaped.glyphs, IsEmpty());
}

TEST_P(TextBackendTest, ShapeRunClustersMatchByteOffsets) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  const auto shaped =
      backend().shapeRun(font, 16.0f, "ABC", 0, 3, false, FontVariant::Normal, false);
  EXPECT_THAT(shaped.glyphs,
              ElementsAre(GlyphClusterIs(0u), GlyphClusterIs(1u), GlyphClusterIs(2u)));
}

TEST_P(TextBackendTest, ShapeRunVerticalLatinUsesSidewaysAdvances) {
  const FontHandle font = loadFont("NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(font));

  const auto shaped =
      backend().shapeRun(font, 64.0f, "Text", 0, 4, true, FontVariant::Normal, false);
  // Both backends report sideways Latin with a positive advance in at least one axis.
  EXPECT_THAT(shaped.glyphs,
              AllOf(SizeIs(4), Each(AllOf(GlyphIndexIs(Gt(0)), HasPositiveAdvanceInEitherAxis()))));
}

TEST_P(TextBackendTest, ShapeRunVerticalCjkUsesFontVerticalMetrics) {
  const FontHandle font = loadFont("MPLUS1p-Regular.ttf", "MPLUS 1p");
  ASSERT_TRUE(static_cast<bool>(font));
  const auto vhea = fontManager_.sfntTable(font, "vhea");
  const auto vmtx = fontManager_.sfntTable(font, "vmtx");
  ASSERT_THAT(vhea.has_value(), testing::IsTrue());
  ASSERT_THAT(vmtx.has_value(), testing::IsTrue());
  ASSERT_GE(vhea->size(), 36u);
  const size_t longMetrics = ReadBe16(*vhea, 34);
  ASSERT_THAT(longMetrics, Gt(0u));

  ASSERT_THAT(backend().fontVMetrics(font).unitsPerEm, testing::Eq(1000));
  const double scale = 64.0 / 1000.0;
  // HarfBuzz positions are 26.6 fixed point, so the full backend rounds to 1/64 px.
  constexpr double kPositionTolerance = 1.0 / 64.0;

  const char* text = "\xE6\x97\xA5\xE6\x9C\xAC";  // 日本
  const auto horizontal =
      backend().shapeRun(font, 1000.0f, text, 0, 6, false, FontVariant::Normal, false);
  ASSERT_THAT(horizontal.glyphs, AllOf(SizeIs(2), Each(GlyphXAdvanceIs(DoubleEq(1000.0)))));
  for (const TextOrientation orientation : {TextOrientation::Mixed, TextOrientation::Upright}) {
    SCOPED_TRACE(orientation == TextOrientation::Mixed ? "mixed" : "upright");
    const auto shaped =
        backend().shapeRun(font, 64.0f, text, 0, 6, true, orientation, FontVariant::Normal, false);
    ASSERT_THAT(shaped.glyphs, SizeIs(2));
    for (const auto& glyph : shaped.glyphs) {
      ASSERT_THAT(glyph.glyphIndex, Gt(0));
      // The vertical origin is the vmtx top side bearing above the glyph box (OpenType's top
      // phantom point), horizontally centered on the glyph's 1000-unit horizontal advance.
      const size_t index = static_cast<size_t>(glyph.glyphIndex);
      const size_t advanceOffset = VmtxAdvanceOffset(index, longMetrics);
      const size_t bearingOffset = VmtxTopSideBearingOffset(index, longMetrics);
      ASSERT_LE(advanceOffset + 2, vmtx->size());
      ASSERT_LE(bearingOffset + 2, vmtx->size());
      // The tested ideographs have on-curve extrema, so the outline box is the glyph box.
      const Path outline = backend().glyphOutline(font, glyph.glyphIndex, 1.0f);
      ASSERT_THAT(outline.commands(), Not(IsEmpty()));
      const double origin =
          -outline.bounds().topLeft.y + static_cast<int16_t>(ReadBe16(*vmtx, bearingOffset));
      EXPECT_THAT(glyph, AllOf(GlyphXAdvanceIs(DoubleEq(0.0)),
                               GlyphYAdvanceIs(DoubleNear(ReadBe16(*vmtx, advanceOffset) * scale,
                                                          kPositionTolerance)),
                               GlyphXOffsetIs(DoubleNear(-500.0 * scale, kPositionTolerance)),
                               GlyphYOffsetIs(DoubleNear(origin * scale, kPositionTolerance))));
    }
  }
}

TEST_P(TextBackendTest, UprightWithoutVerticalMetricsSharesTheHorizontalAscent) {
  const FontHandle font = loadFont("NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(font));
  ASSERT_THAT(fontManager_.sfntTable(font, "vmtx").has_value(), testing::IsFalse());
  ASSERT_THAT(fontManager_.sfntTable(font, "VORG").has_value(), testing::IsFalse());
  const auto hhea = fontManager_.sfntTable(font, "hhea");
  ASSERT_THAT(hhea.has_value(), testing::IsTrue());
  ASSERT_GE(hhea->size(), 6u);
  const int ascent = static_cast<int16_t>(ReadBe16(*hhea, 4));
  const auto os2 = fontManager_.sfntTable(font, "OS/2");
  ASSERT_THAT(os2.has_value(), testing::IsTrue());
  ASSERT_GE(os2->size(), 78u);
  const int unitsPerEm = backend().fontVMetrics(font).unitsPerEm;
  ASSERT_THAT(unitsPerEm, Gt(0));
  // The typographic line height provides the advance even when the ink heights differ.
  const int cell =
      static_cast<int16_t>(ReadBe16(*os2, 68)) - static_cast<int16_t>(ReadBe16(*os2, 70));
  // FreeType scales vertical metrics at an integer ppem, so use an integral size.
  constexpr float kFontSizePx = 64.0f;
  const double scale = kFontSizePx / unitsPerEm;
  // Allow the backends' conversion to scaled font positions.
  constexpr double kPositionTolerance = 2.0 / 64.0;
  const std::string text = "Hx ";
  const auto horizontal = backend().shapeRun(font, kFontSizePx, text, 0, text.size(), false,
                                             FontVariant::Normal, false);
  const auto shaped = backend().shapeRun(font, kFontSizePx, text, 0, text.size(), true,
                                         TextOrientation::Upright, FontVariant::Normal, false);
  ASSERT_THAT(horizontal.glyphs, SizeIs(3));
  ASSERT_THAT(shaped.glyphs, SizeIs(3));
  for (size_t i = 0; i < shaped.glyphs.size(); ++i) {
    const auto& glyph = shaped.glyphs[i];
    SCOPED_TRACE(text.substr(i, 1));
    ASSERT_THAT(glyph.glyphIndex, Gt(0));
    EXPECT_THAT(
        glyph,
        AllOf(GlyphYAdvanceIs(DoubleNear(cell * scale, kPositionTolerance)),
              GlyphXOffsetIs(DoubleNear(-horizontal.glyphs[i].xAdvance / 2.0, kPositionTolerance)),
              GlyphYOffsetIs(DoubleNear(ascent * scale, kPositionTolerance))));
  }
}

TEST(TextBackendSimpleVerticalOrigin, UprightMarksUseTheirUprightBaseFrame) {
  Registry registry;
  FontManager manager(registry);
  TextBackendSimple backend(manager, registry);
  const FontHandle font = LoadResvgFont(manager, "NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  // U+0301 has no advance; after an inked base and after an empty one it is drawn at the base's
  // origin: the same cross offset and one base advance back along the inline axis.
  for (const std::string_view base : {"x", " "}) {
    SCOPED_TRACE(base);
    const std::string text = std::string(base) + "\xCC\x81";
    const auto shaped = backend.shapeRun(font, 32.0f, text, 0, text.size(), true,
                                         TextOrientation::Upright, FontVariant::Normal, false);
    ASSERT_THAT(shaped.glyphs, SizeIs(2));
    const auto& baseGlyph = shaped.glyphs[0];
    EXPECT_THAT(shaped.glyphs[1],
                AllOf(GlyphYAdvanceIs(DoubleEq(0.0)), GlyphXOffsetIs(DoubleEq(baseGlyph.xOffset)),
                      GlyphYOffsetIs(DoubleEq(baseGlyph.yOffset - baseGlyph.yAdvance))));
  }
}

TEST(TextBackendFullCapabilities, PackagedCffFontShapesOutlinesAndRendersThroughFreeType) {
  auto document = instantiateSubtree(
      "<text x='16' y='48' font-size='24' font-family='CffControl'>A</text>", {}, {80, 80});
  FontManager manager(document.registry());
  const FontHandle packaged = manager.fallbackFont();
  ASSERT_THAT(manager.sfntTable(packaged, "CFF ").has_value(), testing::IsTrue());
  const auto bytes = manager.fontData(packaged);
  css::FontFace face;
  face.familyName = RcString("CffControl");
  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Data;
  source.payload = std::make_shared<const std::vector<uint8_t>>(bytes.begin(), bytes.end());
  source.trusted = true;
  face.sources.push_back(std::move(source));
  manager.addFontFace(face);
  const FontHandle selected = manager.findFont("CffControl");
  ASSERT_THAT(manager.sfntTable(selected, "CFF ").has_value(), testing::IsTrue());
  TextBackendFull backend(manager, document.registry());
  const auto shaped =
      backend.shapeRun(selected, 24.0f, "A", 0, 1, false, FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, SizeIs(1));
  ASSERT_THAT(shaped.glyphs[0].glyphIndex, Gt(0));
  ASSERT_THAT(shaped.glyphs[0].xOffset, DoubleEq(0.0));
  ASSERT_THAT(shaped.glyphs[0].yOffset, DoubleEq(0.0));
  const Path outline = backend.glyphOutline(selected, shaped.glyphs[0].glyphIndex,
                                            backend.scaleForEmToPixels(selected, 24.0f));
  ASSERT_THAT(outline.commands(), Not(IsEmpty()));
  auto control =
      instantiateSubtree("<path id='glyph' transform='translate(16 48)'/>", {}, {80, 80});
  auto element = control.querySelector("#glyph");
  ASSERT_THAT(element.has_value(), testing::IsTrue());
  auto path = element->cast<SVGPathElement>();
  path.setSpline(outline);
  auto empty = instantiateSubtree("<g/>", {}, {80, 80});
  const auto actual = RenderDocumentWithActiveBackend(document);
  const auto expected = RenderDocumentWithActiveBackend(control);
  const auto transparent = RenderDocumentWithActiveBackend(empty);
  ASSERT_THAT(actual.pixels, Not(IsEmpty()));
  ASSERT_THAT(expected.dimensions, testing::Eq(Vector2i(80, 80)));
  EXPECT_NONFATAL_FAILURE(
      editor::tests::CompareBitmapToBitmap(expected, transparent, "cff_visible_outline",
                                           editor::tests::PixelmatchIdentityParams()),
      "pixels differ");
  editor::tests::CompareBitmapToBitmap(actual, expected, "cff_text_outline",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(TextBackendFullVerticalOrigin, SvgUprightPlacementUsesAvailableVerticalOrigins) {
  auto makeDocument = [](std::string_view content) {
    auto document = instantiateSubtree(content, {}, {200, 220});
    auto& registry = document.registry();
    auto& manager = registry.ctx().emplace<FontManager>(registry);
    css::FontFace face;
    face.familyName = RcString("VerticalOriginControl");
    css::FontFaceSource source;
    source.kind = css::FontFaceSource::Kind::Data;
    source.payload =
        std::make_shared<const std::vector<uint8_t>>(ReadResvgFontBytes("MPLUS1p-Regular.ttf"));
    source.trusted = true;
    face.sources.push_back(std::move(source));
    manager.addFontFace(face);
    registry.ctx().emplace<TextEngine>(manager, registry,
                                       std::make_unique<TextBackendFull>(manager, registry));
    return document;
  };
  auto document = makeDocument(
      "<text x='100' y='10' font-size='100' font-family='VerticalOriginControl' "
      "writing-mode='tb'>日本</text>");
  auto& manager = document.registry().ctx().get<FontManager>();
  const FontHandle font = manager.findFont("VerticalOriginControl");
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  const auto vhea = manager.sfntTable(font, "vhea");
  const auto vmtx = manager.sfntTable(font, "vmtx");
  ASSERT_THAT(vhea.has_value(), testing::IsTrue());
  ASSERT_THAT(vmtx.has_value(), testing::IsTrue());
  ASSERT_GE(vhea->size(), 36u);
  const size_t longMetrics = ReadBe16(*vhea, 34);
  TextBackendFull backend(manager, document.registry());
  const std::string text = "日本";
  const auto shaped = backend.shapeRun(font, 1000.0f, text, 0, text.size(), true,
                                       TextOrientation::Mixed, FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, SizeIs(2));
  EXPECT_THAT(backend.fontVMetrics(font).unitsPerEm, testing::Eq(1000));
  EXPECT_THAT(shaped.glyphs[0].glyphIndex, testing::Eq(4868));
  EXPECT_THAT(shaped.glyphs[1].glyphIndex, testing::Eq(4977));
  for (const auto& glyph : shaped.glyphs) {
    ASSERT_THAT(glyph.glyphIndex, Gt(0));
    EXPECT_THAT(glyph.xOffset, DoubleEq(-500));
    const size_t index = static_cast<size_t>(glyph.glyphIndex);
    ASSERT_THAT(longMetrics, Gt(0u));
    const size_t advanceOffset = VmtxAdvanceOffset(index, longMetrics);
    const size_t bearingOffset = VmtxTopSideBearingOffset(index, longMetrics);
    ASSERT_LE(advanceOffset + 2, vmtx->size());
    ASSERT_LE(bearingOffset + 2, vmtx->size());
    EXPECT_THAT(ReadBe16(*vmtx, advanceOffset), testing::Eq(1000));
    const std::string character = index == 4868 ? "日" : "本";
    const auto horizontal = backend.shapeRun(font, 1000.0f, character, 0, character.size(), false,
                                             FontVariant::Normal, false);
    ASSERT_THAT(horizontal.glyphs, SizeIs(1));
    EXPECT_THAT(horizontal.glyphs[0].xAdvance, DoubleEq(1000));
    const Path outline = backend.glyphOutline(font, glyph.glyphIndex, 1.0f);
    ASSERT_THAT(outline.commands(), Not(IsEmpty()));
    EXPECT_THAT(-outline.bounds().topLeft.y + static_cast<int16_t>(ReadBe16(*vmtx, bearingOffset)),
                DoubleEq(860));
  }
  // vmtx origin 860 and advance 1000 at 100px place alphabetic origins at 96 and 196.
  auto control = makeDocument(
      "<g font-size='100' font-family='VerticalOriginControl'>"
      "<text x='50' y='96'>日</text><text x='50' y='196'>本</text></g>");
  auto shifted = makeDocument(
      "<g font-size='100' font-family='VerticalOriginControl'>"
      "<text x='50' y='98'>日</text><text x='50' y='198'>本</text></g>");
  auto empty = instantiateSubtree("<g/>", {}, {200, 220});
  const auto actual = RenderDocumentWithActiveBackend(document);
  const auto expected = RenderDocumentWithActiveBackend(control);
  const auto displaced = RenderDocumentWithActiveBackend(shifted);
  const auto transparent = RenderDocumentWithActiveBackend(empty);
  ASSERT_THAT(actual.dimensions, testing::Eq(Vector2i(200, 220)));
  ASSERT_THAT(actual.pixels, Not(IsEmpty()));
  ASSERT_THAT(expected.pixels, Not(IsEmpty()));
  EXPECT_NONFATAL_FAILURE(
      editor::tests::CompareBitmapToBitmap(expected, transparent, "vertical_origin_visible_ink",
                                           editor::tests::PixelmatchIdentityParams()),
      "pixels differ");
  EXPECT_NONFATAL_FAILURE(
      editor::tests::CompareBitmapToBitmap(actual, displaced, "vertical_origin_displaced_control",
                                           editor::tests::PixelmatchIdentityParams()),
      "pixels differ");
  editor::tests::CompareBitmapToBitmap(actual, expected, "vertical_origin_svg_placement",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(TextBackendFullVerticalOrigin, TrustedBitmapVerticalMetricsRemainAvailable) {
  Registry registry;
  FontManager manager(registry);
  TextBackendFull backend(manager, registry);
  const FontHandle font = LoadResvgFont(manager, "NotoColorEmoji.ttf", "Noto Color Emoji");
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  ASSERT_THAT(backend.isBitmapOnly(font), testing::IsTrue());
  const std::string text = "\xf0\x9f\x98\x81\xf0\x9f\x98\x82";
  const auto shaped = backend.shapeRun(font, 32.0f, text, 0, text.size(), true,
                                       TextOrientation::Upright, FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, SizeIs(2));
  EXPECT_THAT(shaped.glyphs[1].glyphIndex, Not(testing::Eq(shaped.glyphs[0].glyphIndex)));
  EXPECT_THAT(shaped.glyphs[1].yAdvance, Gt(0.0));
  EXPECT_THAT(shaped.glyphs[0].glyphIndex, Gt(0));
  EXPECT_THAT(shaped.glyphs[0].yAdvance, Gt(0.0));
  RecordProperty("verticalAdvance", std::to_string(shaped.glyphs[0].yAdvance));
  RecordProperty("originX", std::to_string(shaped.glyphs[0].xOffset));
  RecordProperty("originY", std::to_string(shaped.glyphs[0].yOffset));
}

TEST(TextBackendFullVerticalOrigin, UntrustedBitmapDoesNotEnterMetricCallbacks) {
  Registry registry;
  FontManager manager(registry);
  TextBackendFull backend(manager, registry);
  const FontHandle font = LoadResvgFont(manager, "NotoColorEmoji.ttf", "Noto Color Emoji", false);
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  const std::string text = "\xf0\x9f\x98\x81\xf0\x9f\x98\x82";
  const auto shaped = backend.shapeRun(font, 32.0f, text, 0, text.size(), true,
                                       TextOrientation::Upright, FontVariant::Normal, false);
  EXPECT_THAT(shaped.glyphs, IsEmpty());
  EXPECT_THAT(backend.delegatedVerticalOriginCallsForTesting(), testing::Eq(0u));
  EXPECT_THAT(backend.delegatedVerticalAdvanceCallsForTesting(), testing::Eq(0u));
}

TEST(TextBackendFullVerticalOrigin, RejectedVerticalMetricBudgetReturnsNoPartialRun) {
  Registry registry;
  FontManager manager(registry);
  TextBackendFull backend(manager, registry);
  const FontHandle font = LoadResvgFont(manager, "MPLUS1p-Regular.ttf", "MPLUS 1p", false);
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  const std::string text = "\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5";
  TextBackend::ShapingContext::WorkBudget work{1024, 8};
  work.glyphOutlineVertices = 0;
  work.glyphOutlineWork = 0;
  TextBackend::ShapingContext context;
  context.byteLength = text.size();
  context.work = &work;
  const auto shaped = backend.shapeRunInContext(font, 20.0f, text, context, 0, text.size(), true,
                                                TextOrientation::Mixed, FontVariant::Normal, false);
  EXPECT_THAT(shaped.glyphs, IsEmpty());
  EXPECT_THAT(work.rejected, testing::IsTrue());
  EXPECT_THAT(backend.delegatedVerticalOriginCallsForTesting(), testing::Eq(0u));
  EXPECT_THAT(backend.delegatedVerticalAdvanceCallsForTesting(), testing::Eq(0u));
}

TEST(TextBackendFullVerticalOrigin, AggregateMetricBudgetStopsRepeatedAndLaterContextWork) {
  Registry registry;
  FontManager manager(registry);
  TextBackendFull backend(manager, registry);
  const FontHandle font = LoadResvgFont(manager, "MPLUS1p-Regular.ttf", "MPLUS 1p", false);
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  const std::string character = "\xe6\x97\xa5";
  const auto probe = backend.shapeRun(font, 20.0f, character, 0, character.size(), false,
                                      FontVariant::Normal, false);
  ASSERT_THAT(probe.glyphs, SizeIs(1));
  const auto complexity = manager.glyphOutlineComplexity(font, probe.glyphs[0].glyphIndex);
  ASSERT_THAT(complexity.has_value(), testing::IsTrue());
  ASSERT_THAT(complexity->work, Gt(0u));
  ASSERT_LE(complexity->maximumVertices, fonts::kMaximumExpandedGlyphVertices / 2);
  ASSERT_LE(complexity->work, fonts::kMaximumGlyphOutlineWork / 2);
  const std::string text = character + character + character;
  TextBackend::ShapingContext::WorkBudget work{4096, 8};
  work.glyphOutlineVertices = complexity->maximumVertices * 2;
  work.glyphOutlineWork = complexity->work * 2;
  TextBackend::ShapingContext context;
  context.byteLength = text.size();
  context.work = &work;
  const auto shaped = backend.shapeRunInContext(font, 20.0f, text, context, 0, text.size(), true,
                                                TextOrientation::Mixed, FontVariant::Normal, false);
  EXPECT_THAT(shaped.glyphs, IsEmpty());
  EXPECT_THAT(work.rejected, testing::IsTrue());
  EXPECT_THAT(backend.delegatedVerticalAdvanceCallsForTesting(), testing::Eq(2u));
  EXPECT_THAT(backend.delegatedVerticalOriginCallsForTesting(), testing::Eq(0u));
  const auto later =
      backend.shapeRunInContext(font, 20.0f, text, context, 0, character.size(), true,
                                TextOrientation::Mixed, FontVariant::Normal, false);
  EXPECT_THAT(later.glyphs, IsEmpty());
  EXPECT_THAT(backend.delegatedVerticalAdvanceCallsForTesting(), testing::Eq(2u));
  EXPECT_THAT(backend.delegatedVerticalOriginCallsForTesting(), testing::Eq(0u));
}

TEST(TextBackendFullVerticalOrigin, MixedSmallCapsRangesKeepTheUprightParentScale) {
  Registry registry;
  FontManager manager(registry);
  TextBackendFull backend(manager, registry);
  const FontHandle font = LoadResvgFont(manager, "MPLUS1p-Regular.ttf", "MPLUS 1p", false);
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  const std::string character = "\xe6\x97\xa5";
  const std::string text = "a" + character + "b";
  for (float size : {20.0f, 64.0f}) {
    SCOPED_TRACE(size);
    const auto control = backend.shapeRun(font, size, character, 0, character.size(), true,
                                          TextOrientation::Mixed, FontVariant::Normal, false);
    const auto mixed = backend.shapeRun(font, size, text, 0, text.size(), true,
                                        TextOrientation::Mixed, FontVariant::SmallCaps, false);
    ASSERT_THAT(control.glyphs, SizeIs(1));
    ASSERT_THAT(mixed.glyphs, SizeIs(3));
    EXPECT_THAT(mixed.glyphs[1].glyphIndex, testing::Eq(control.glyphs[0].glyphIndex));
    EXPECT_THAT(mixed.glyphs[1].xOffset, DoubleEq(control.glyphs[0].xOffset));
    EXPECT_THAT(mixed.glyphs[1].yOffset, DoubleEq(control.glyphs[0].yOffset));
    EXPECT_THAT(mixed.glyphs[1].yAdvance, DoubleEq(control.glyphs[0].yAdvance));
  }
  EXPECT_THAT(backend.delegatedVerticalOriginCallsForTesting(), Gt(0u));
  EXPECT_THAT(backend.delegatedVerticalAdvanceCallsForTesting(), Gt(0u));
}

TEST_P(TextBackendTest, MixedUsesEachGlyphVerticalSideBearing) {
  FontManager& manager = fontManager_;
  TextBackend& backend = this->backend();
  const FontHandle original = LoadResvgFont(manager, "MPLUS1p-Regular.ttf", "MPLUS 1p");
  ASSERT_THAT(static_cast<bool>(original), testing::IsTrue());
  const std::string text = "\xe6\x97\xa5\xe6\x9c\xac";
  const auto originalRun = backend.shapeRun(original, 1000.0f, text, 0, text.size(), true,
                                            TextOrientation::Upright, FontVariant::Normal, false);
  ASSERT_THAT(originalRun.glyphs, SizeIs(2));
  const auto vhea = manager.sfntTable(original, "vhea");
  ASSERT_THAT(vhea.has_value(), testing::IsTrue());
  ASSERT_GE(vhea->size(), 36u);
  const size_t longMetrics = ReadBe16(*vhea, 34);
  std::vector<uint8_t> bytes = ReadResvgFontBytes("MPLUS1p-Regular.ttf");
  size_t vmtxOffset = 0;
  size_t vmtxSize = 0;
  for (size_t i = 0; i < ReadBe16(bytes, 4); ++i) {
    const size_t entry = 12 + i * 16;
    ASSERT_LE(entry + 16, bytes.size());
    if (ReadBe32(bytes, entry) == 0x766d7478) {
      vmtxOffset = ReadBe32(bytes, entry + 8);
      vmtxSize = ReadBe32(bytes, entry + 12);
      break;
    }
  }
  ASSERT_GT(vmtxOffset, 0u);
  ASSERT_LE(vmtxOffset + vmtxSize, bytes.size());
  constexpr std::array<uint16_t, 2> kSideBearings{140, 210};
  std::array<double, 2> expectedOrigins{};
  for (size_t i = 0; i < 2; ++i) {
    const size_t glyph = originalRun.glyphs[i].glyphIndex;
    const size_t bearingOffset = VmtxTopSideBearingOffset(glyph, longMetrics);
    ASSERT_LE(bearingOffset + 2, vmtxSize);
    WriteBe16(&bytes, vmtxOffset + bearingOffset, kSideBearings[i]);
    const Path outline = backend.glyphOutline(original, static_cast<int>(glyph), 1.0f);
    ASSERT_THAT(outline.commands(), Not(IsEmpty()));
    expectedOrigins[i] = -outline.bounds().topLeft.y + kSideBearings[i];
  }
  const FontHandle changed = manager.loadFontData(bytes, FontDataTrust::Trusted);
  ASSERT_THAT(static_cast<bool>(changed), testing::IsTrue());
  const auto shaped = backend.shapeRun(changed, 1000.0f, text, 0, text.size(), true,
                                       TextOrientation::Mixed, FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, SizeIs(2));
  for (size_t i = 0; i < 2; ++i) {
    EXPECT_THAT(shaped.glyphs[i].yOffset, DoubleEq(expectedOrigins[i]));
  }
}

TEST(TextBackendFullVerticalOrigin, MixedPreservesUprightCombiningMarkPositioning) {
  Registry registry;
  FontManager manager(registry);
  TextBackendFull backend(manager, registry);
  const FontHandle font = LoadResvgFont(manager, "MPLUS1p-Regular.ttf", "MPLUS 1p", false);
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  const std::string text = "\xe6\x97\xa5\xcc\x81\xe6\x9c\xac";
  for (float size : {20.0f, 64.0f}) {
    SCOPED_TRACE(size);
    const auto upright = backend.shapeRun(font, size, text, 0, text.size(), true,
                                          TextOrientation::Upright, FontVariant::Normal, false);
    const auto mixed = backend.shapeRun(font, size, text, 0, text.size(), true,
                                        TextOrientation::Mixed, FontVariant::Normal, false);
    ASSERT_THAT(upright.glyphs, SizeIs(3));
    ASSERT_THAT(mixed.glyphs, SizeIs(3));
    ASSERT_THAT(upright.glyphs[1].glyphIndex, Gt(0));
    ASSERT_THAT(upright.glyphs[1].yAdvance, DoubleEq(0.0));
    for (size_t i = 0; i < 3; ++i) {
      EXPECT_THAT(mixed.glyphs[i].yOffset, DoubleEq(upright.glyphs[i].yOffset));
      EXPECT_THAT(mixed.glyphs[i].xOffset, DoubleEq(upright.glyphs[i].xOffset));
      EXPECT_THAT(mixed.glyphs[i].yAdvance, DoubleEq(upright.glyphs[i].yAdvance));
    }
  }
  EXPECT_THAT(backend.delegatedVerticalOriginCallsForTesting(), Gt(0u));
  EXPECT_THAT(backend.delegatedVerticalAdvanceCallsForTesting(), Gt(0u));
}

// Noto Sans has a GSUB smcp feature; MPLUS 1p and the Public Sans fallback do not.
TEST_P(TextBackendTest, DetectsSmallCapsFeature) {
  const FontHandle notoSans = loadFont("NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(notoSans));
  EXPECT_TRUE(backend().hasSmallCapsFeature(notoSans));

  const FontHandle mplus = loadFont("MPLUS1p-Regular.ttf", "MPLUS 1p");
  ASSERT_TRUE(static_cast<bool>(mplus));
  EXPECT_FALSE(backend().hasSmallCapsFeature(mplus));

  const FontHandle fallback = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(fallback));
  EXPECT_FALSE(backend().hasSmallCapsFeature(fallback));
}

TEST_P(TextBackendTest, SmallCapsSynthesizesScaleForFontWithoutSmcp) {
  // MPLUS 1p doesn't have native smcp - both backends synthesize.
  const FontHandle font = loadFont("MPLUS1p-Regular.ttf", "MPLUS 1p");
  ASSERT_TRUE(static_cast<bool>(font));
  ASSERT_FALSE(backend().hasSmallCapsFeature(font));

  const auto shaped =
      backend().shapeRun(font, 32.0f, "aB", 0, 2, false, FontVariant::SmallCaps, false);
  EXPECT_THAT(shaped.glyphs,
              ElementsAre(GlyphFontSizeScaleIs(Lt(1.0f)), GlyphFontSizeScaleIs(FloatEq(1.0f))));
}

// ── Cross-span kerning ──────────────────────────────────────────────────────

TEST_P(TextBackendTest, CrossSpanKernReturnsZeroForInvalidFont) {
  EXPECT_DOUBLE_EQ(
      backend().crossSpanKern(FontHandle{}, 16.0f, FontHandle{}, 16.0f, 'A', 'V', false), 0.0);
}

TEST_P(TextBackendTest, CrossSpanKernReturnsFiniteValue) {
  const FontHandle font = loadFont("NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(font));

  const double kern = backend().crossSpanKern(font, 16.0f, font, 16.0f, 'A', 'V', false);
  EXPECT_TRUE(std::isfinite(kern));
}

// ── .notdef glyph ───────────────────────────────────────────────────────────

TEST_P(TextBackendTest, GlyphOutlineForNotdefDoesNotCrash) {
  const FontHandle font = fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));

  const float scale = backend().scaleForEmToPixels(font, 16.0f);
  // Glyph index 0 is .notdef - may or may not have an outline. Just verify no crash.
  const Path path = backend().glyphOutline(font, 0, scale);
  (void)path;
}

INSTANTIATE_TEST_SUITE_P(Backends, TextBackendTest,
                         testing::Values(BackendType::Simple, BackendType::Full), BackendName);

// ═══════════════════════════════════════════════════════════════════════════
// Capability tests - behaviour that differs between backends.
// ═══════════════════════════════════════════════════════════════════════════

// ── Simple backend: no advanced capabilities ────────────────────────────────

TEST(TextBackendSimpleCapabilities, IsCursiveAlwaysFalse) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendSimple backend(fontManager, registry);

  EXPECT_FALSE(backend.isCursive('A'));
  EXPECT_FALSE(backend.isCursive(0x0627));  // Arabic alef
  EXPECT_FALSE(backend.isCursive(0x4E00));  // CJK
}

TEST(TextBackendSimpleCapabilities, BitmapGlyphAlwaysNullopt) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendSimple backend(fontManager, registry);

  const FontHandle font = fontManager.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(font));
  EXPECT_FALSE(backend.bitmapGlyph(font, 1, 1.0f).has_value());
}

// ── Full backend: advanced capabilities ─────────────────────────────────────

TEST(TextBackendFullCapabilities, DetectsCursiveScripts) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull backend(fontManager, registry);

  struct JoiningScriptCase {
    std::string_view script;
    uint32_t codepoint;
  };
  constexpr std::array kJoining = {
      JoiningScriptCase{"Arabic", 0x0627},
      JoiningScriptCase{"Arabic Supplement", 0x0750},
      JoiningScriptCase{"Arabic Extended-A", 0x08A0},
      JoiningScriptCase{"Arabic Presentation-A", 0xFB54},
      JoiningScriptCase{"Arabic Presentation-B", 0xFE91},
      JoiningScriptCase{"Syriac", 0x0710},
      JoiningScriptCase{"Syriac Supplement", 0x0860},
      JoiningScriptCase{"N'Ko", 0x07CA},
      JoiningScriptCase{"Mandaic", 0x0847},
  };
  for (const auto& sample : kJoining) {
    SCOPED_TRACE(sample.script);
    EXPECT_TRUE(backend.isCursive(sample.codepoint));
  }
  EXPECT_FALSE(backend.isCursive('A'));     // Latin
  EXPECT_FALSE(backend.isCursive(0x4E00));  // CJK
  EXPECT_FALSE(backend.isCursive(0x0780));  // Thaana is right-to-left but non-cursive
  EXPECT_FALSE(backend.isCursive(0xFF00));  // Just beyond Arabic Presentation Forms-B
}

TEST(TextBackendFullCapabilities, TinyFontAdvancesStayInScale) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendSimple simpleBackend(fontManager, registry);
  TextBackendFull fullBackend(fontManager, registry);

  const FontHandle font = LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(font));

  constexpr std::string_view kText = "Some long text";
  const auto simpleShaped = simpleBackend.shapeRun(font, 0.24f, kText, 0, kText.size(), false,
                                                   FontVariant::Normal, false);
  const auto fullShaped =
      fullBackend.shapeRun(font, 0.24f, kText, 0, kText.size(), false, FontVariant::Normal, false);

  ASSERT_THAT(fullShaped.glyphs, Pointwise(AdvanceNear(0.5), simpleShaped.glyphs));

  double simpleAdvance = 0.0;
  for (const auto& glyph : simpleShaped.glyphs) {
    simpleAdvance += glyph.xAdvance;
  }

  double fullAdvance = 0.0;
  for (const auto& glyph : fullShaped.glyphs) {
    fullAdvance += glyph.xAdvance;
  }

  EXPECT_NEAR(fullAdvance, simpleAdvance, 0.5)
      << "simpleAdvance=" << simpleAdvance << ", fullAdvance=" << fullAdvance;
}

/// Reads the big-endian 16-bit value at \p offset of \p table, or nullopt past its end.
std::optional<uint32_t> ReadU16(std::span<const uint8_t> table, size_t offset) {
  if (offset + 2 > table.size()) {
    return std::nullopt;
  }
  return (static_cast<uint32_t>(table[offset]) << 8) | table[offset + 1];
}

/// The unkerned advance of \p glyph at \p sizePx read straight from the font: its `hmtx` advance
/// width scaled by size over `head` units per em. Glyphs past the last long metric share its
/// advance.
std::optional<double> HmtxAdvancePx(const FontManager& fontManager, FontHandle font, int glyph,
                                    double sizePx) {
  const auto head = fontManager.sfntTable(font, "head");
  const auto hhea = fontManager.sfntTable(font, "hhea");
  const auto hmtx = fontManager.sfntTable(font, "hmtx");
  if (!head || !hhea || !hmtx || glyph < 0) {
    return std::nullopt;
  }
  const auto unitsPerEm = ReadU16(*head, 18);
  const auto longMetrics = ReadU16(*hhea, 34);
  if (!unitsPerEm || *unitsPerEm == 0 || !longMetrics || *longMetrics == 0) {
    return std::nullopt;
  }
  const uint32_t metric = std::min(static_cast<uint32_t>(glyph), *longMetrics - 1);
  const auto advance = ReadU16(*hmtx, metric * 4u);
  if (!advance) {
    return std::nullopt;
  }
  return static_cast<double>(*advance) * sizePx / static_cast<double>(*unitsPerEm);
}

/// The `hmtx` oracle advance of each glyph at \p sizePx times its small-caps scale.
std::vector<double> HmtxAdvances(const FontManager& fontManager, FontHandle font,
                                 const std::vector<TextBackend::ShapedGlyph>& glyphs,
                                 double sizePx) {
  std::vector<double> advances;
  for (const auto& glyph : glyphs) {
    advances.push_back(
        HmtxAdvancePx(fontManager, font, glyph.glyphIndex, sizePx * glyph.fontSizeScale)
            .value_or(-1.0));
  }
  return advances;
}

MATCHER_P(XAdvanceNear, tolerance, "") {
  return testing::ExplainMatchResult(DoubleNear(std::get<1>(arg), tolerance),
                                     std::get<0>(arg).xAdvance, result_listener);
}

MATCHER_P2(BoxNear, expected, tolerance,
           "is within " + testing::PrintToString(tolerance) + " of " +
               testing::PrintToString(expected)) {
  return std::abs(arg.topLeft.x - expected.topLeft.x) <= tolerance &&
         std::abs(arg.topLeft.y - expected.topLeft.y) <= tolerance &&
         std::abs(arg.bottomRight.x - expected.bottomRight.x) <= tolerance &&
         std::abs(arg.bottomRight.y - expected.bottomRight.y) <= tolerance;
}

/// Bound on the full tier's error for one advance or outline coordinate at \p sizePx, with
/// margin. FreeType rounds to 1/64 pixel of the face, which is 16 times text up to 128 px (an
/// error of at most 1/2048 px), and otherwise a whole multiple of the text size (at most
/// 1/128 px).
double FullTierPrecisionPx(double sizePx) {
  return sizePx <= 128.0 ? 1.0 / 512.0 : 1.0 / 64.0;
}

// Unkerned advances are the font's `hmtx` widths scaled linearly to the requested size, including
// fractional sizes, large sizes whose advances exceed 2048 px, and sizes past 4096 px. Amiri rounds
// its scaled size to whole pixels per em (head flags bit 3) and Noto Sans does not, so both
// FreeType scaling paths are covered.
TEST(TextBackendFullCapabilities, AdvancesKeepFontUnitPrecision) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull fullBackend(fontManager, registry);

  constexpr std::string_view kText = "Hamburgefonstiv";
  const std::array<std::array<const char*, 2>, 2> kFonts{{
      {"Amiri-Regular.ttf", "Amiri"},
      {"NotoSans-Regular.ttf", "Noto Sans"},
  }};
  for (const auto& [file, family] : kFonts) {
    const FontHandle font = LoadResvgFont(fontManager, file, family);
    ASSERT_TRUE(static_cast<bool>(font)) << family;
    for (float size : {14.0f, 14.3f, 300.3f, 3000.3f, 4096.0f, 4500.7f}) {
      SCOPED_TRACE(testing::Message() << family << " at " << size << " px");
      const auto shaped = fullBackend.shapeRunNoKerning(font, size, kText, 0, kText.size(), false,
                                                        FontVariant::Normal, false);
      ASSERT_THAT(shaped.glyphs, SizeIs(kText.size()));
      EXPECT_THAT(shaped.glyphs, Pointwise(XAdvanceNear(FullTierPrecisionPx(size)),
                                           HmtxAdvances(fontManager, font, shaped.glyphs, size)));
    }
  }
}

// A small units-per-em makes the existing Noto Sans advances wide in em units. At text size
// 96 the advances fit in signed 16.16, but at a 16x face size they would overflow it.
TEST(TextBackendFullCapabilities, WideAdvancesKeepTheirFontUnitWidth) {
  std::vector<uint8_t> bytes = ReadResvgFontBytes("NotoSans-Regular.ttf");
  ASSERT_GE(bytes.size(), 12u);
  for (size_t i = 0; i < ReadBe16(bytes, 4); ++i) {
    const size_t entry = 12 + i * 16;
    ASSERT_LE(entry + 16, bytes.size());
    if (ReadBe32(bytes, entry) == 0x68656164) {  // head
      const size_t offset = ReadBe32(bytes, entry + 8);
      ASSERT_LE(offset + 20, bytes.size());
      WriteBe16(&bytes, offset + 18, 32);  // unitsPerEm
      break;
    }
  }

  Registry registry;
  FontManager manager(registry);
  TextBackendFull backend(manager, registry);
  const FontHandle font = manager.loadFontData(bytes, FontDataTrust::Trusted);
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  constexpr float kSize = 96.0f;
  const auto run =
      backend.shapeRunNoKerning(font, kSize, "WW", 0, 2, false, FontVariant::Normal, false);
  ASSERT_THAT(run.glyphs, SizeIs(2));
  const auto expected = HmtxAdvances(manager, font, run.glyphs, kSize);
  ASSERT_THAT(expected, Each(Gt(2048.0)));
  EXPECT_THAT(run.glyphs, Pointwise(XAdvanceNear(1.0 / 64.0), expected));
}

// Synthesized small caps shape at 0.8 of the text size on the same face scale as the full-size
// glyphs, so their advances stay the scaled `hmtx` widths, including where the text size is
// oversampled less than the reduced size could be.
TEST(TextBackendFullCapabilities, SynthesizedSmallCapsShareTheTextFaceScale) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull backend(fontManager, registry);

  // MPLUS 1p has no smcp feature, so lowercase letters are synthesized.
  const FontHandle font = LoadResvgFont(fontManager, "MPLUS1p-Regular.ttf", "MPLUS 1p");
  ASSERT_TRUE(static_cast<bool>(font));
  ASSERT_FALSE(backend.hasSmallCapsFeature(font));
  for (float size : {14.3f, 150.3f, 4500.7f}) {
    SCOPED_TRACE(testing::Message() << size << " px");
    const auto shaped =
        backend.shapeRunNoKerning(font, size, "aBc", 0, 3, false, FontVariant::SmallCaps, false);
    ASSERT_THAT(shaped.glyphs,
                ElementsAre(GlyphFontSizeScaleIs(Lt(1.0f)), GlyphFontSizeScaleIs(FloatEq(1.0f)),
                            GlyphFontSizeScaleIs(Lt(1.0f))));
    EXPECT_THAT(shaped.glyphs, Pointwise(XAdvanceNear(FullTierPrecisionPx(size)),
                                         HmtxAdvances(fontManager, font, shaped.glyphs, size)));
  }
}

MATCHER_P(SameGlyphAndAdvanceAs, tolerance, "") {
  const auto& [actual, expected] = arg;
  return testing::ExplainMatchResult(
      AllOf(Field("glyphIndex", &TextBackend::ShapedGlyph::glyphIndex, expected.glyphIndex),
            GlyphFontSizeScaleIs(FloatEq(expected.fontSizeScale)),
            Field("xAdvance", &TextBackend::ShapedGlyph::xAdvance,
                  DoubleNear(expected.xAdvance, tolerance))),
      actual, result_listener);
}

// A face with native small caps draws its own small-cap glyphs at the full text size, so the
// simple backend applies the face's smcp substitutions as HarfBuzz does for the full backend,
// rather than drawing capitals at 0.8 of the size.
TEST(TextBackendSimpleCapabilities, NativeSmallCapsMatchTheFullBackend) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendSimple simpleBackend(fontManager, registry);
  TextBackendFull fullBackend(fontManager, registry);

  const FontHandle font = LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(font));
  constexpr std::string_view kText = "Text Ab";
  constexpr float kSize = 64.0f;
  const auto full = fullBackend.shapeRunNoKerning(font, kSize, kText, 0, kText.size(), false,
                                                  FontVariant::SmallCaps, false);
  const auto normal = fullBackend.shapeRunNoKerning(font, kSize, kText, 0, kText.size(), false,
                                                    FontVariant::Normal, false);
  ASSERT_THAT(full.glyphs, SizeIs(kText.size()));
  ASSERT_THAT(normal.glyphs, SizeIs(kText.size()));
  // The lowercase letters take small-cap glyphs; the capitals and the space keep theirs.
  for (size_t i : {1u, 2u, 3u, 6u}) {
    EXPECT_THAT(full.glyphs[i].glyphIndex, Not(testing::Eq(normal.glyphs[i].glyphIndex)))
        << "glyph " << i;
  }
  for (size_t i : {0u, 4u, 5u}) {
    EXPECT_THAT(full.glyphs[i].glyphIndex, testing::Eq(normal.glyphs[i].glyphIndex))
        << "glyph " << i;
  }

  const auto simple = simpleBackend.shapeRunNoKerning(font, kSize, kText, 0, kText.size(), false,
                                                      FontVariant::SmallCaps, false);
  EXPECT_THAT(simple.glyphs,
              Pointwise(SameGlyphAndAdvanceAs(FullTierPrecisionPx(kSize)), full.glyphs));
}

/// Byte offset of the table tagged \p tag (big-endian) in the sfnt \p bytes, if present.
std::optional<size_t> SfntTableOffset(std::span<const uint8_t> bytes, uint32_t tag) {
  for (size_t i = 0; bytes.size() >= 12 && i < ReadBe16(bytes, 4); ++i) {
    const size_t entry = 12 + i * 16;
    if (entry + 16 <= bytes.size() && ReadBe32(bytes, entry) == tag) {
      return ReadBe32(bytes, entry + 8);
    }
  }
  return std::nullopt;
}

/// Registers Noto Sans with no features in one script's default language system.
FontHandle RegisterCapsFaceWithoutScriptFeatures(FontManager& manager, uint32_t scriptTag,
                                                 bool keepNamedLanguages = false) {
  std::vector<uint8_t> bytes = ReadResvgFontBytes("NotoSans-Regular.ttf");
  const auto gsub = SfntTableOffset(bytes, 0x47535542);  // GSUB
  if (!gsub) {
    return {};
  }
  const size_t scripts = *gsub + ReadBe16(bytes, *gsub + 4);
  for (size_t i = 0; i < ReadBe16(bytes, scripts); ++i) {
    const size_t record = scripts + 2 + i * 6;
    if (ReadBe32(bytes, record) != scriptTag) {
      continue;
    }
    const size_t script = scripts + ReadBe16(bytes, record + 4);
    const size_t defaultOffset = ReadBe16(bytes, script);
    if (defaultOffset == 0) {
      return {};
    }
    const size_t langSys = script + defaultOffset;
    if (!keepNamedLanguages) {
      WriteBe16(&bytes, script + 2, 0);
    }
    WriteBe16(&bytes, langSys + 4, 0);  // No features in the default language.
    css::FontFace face;
    face.familyName = RcString("CapsScriptControl");
    css::FontFaceSource source;
    source.kind = css::FontFaceSource::Kind::Data;
    source.payload = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    source.trusted = true;
    face.sources.push_back(std::move(source));
    manager.addFontFace(face);
    return manager.findFont("CapsScriptControl");
  }
  return {};
}

void CheckLatinSmallCapsScript(bool latinHasFeature) {
  Registry registry;
  FontManager fontManager(registry);
  const FontHandle original = LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "CapsReference");
  const FontHandle modified = RegisterCapsFaceWithoutScriptFeatures(
      fontManager, latinHasFeature ? 0x44464C54u : 0x6C61746Eu);  // DFLT or latn.
  ASSERT_TRUE(static_cast<bool>(original));
  ASSERT_TRUE(static_cast<bool>(modified));
  TextBackendFull reference(fontManager, registry);
  const auto native =
      reference.shapeRun(original, 64.f, "a", 0, 1, false, FontVariant::SmallCaps, false);
  const auto lower =
      reference.shapeRun(original, 64.f, "a", 0, 1, false, FontVariant::Normal, false);
  const auto upper =
      reference.shapeRun(original, 64.f, "A", 0, 1, false, FontVariant::Normal, false);
  ASSERT_THAT(native.glyphs, SizeIs(1));
  ASSERT_THAT(lower.glyphs, SizeIs(1));
  ASSERT_THAT(upper.glyphs, SizeIs(1));
  ASSERT_THAT(native.glyphs[0].fontSizeScale, FloatEq(1.0f));
  ASSERT_THAT(native.glyphs[0].glyphIndex, Not(testing::Eq(lower.glyphs[0].glyphIndex)));
  ASSERT_THAT(native.glyphs[0].glyphIndex, Not(testing::Eq(upper.glyphs[0].glyphIndex)));

  for (const bool useFull : {false, true}) {
    SCOPED_TRACE(useFull ? "full tier" : "simple tier");
    std::unique_ptr<TextBackend> backend;
    if (useFull) {
      backend = std::make_unique<TextBackendFull>(fontManager, registry);
    } else {
      backend = std::make_unique<TextBackendSimple>(fontManager, registry);
    }
    const auto direct =
        backend->shapeRun(modified, 64.f, "a", 0, 1, false, FontVariant::SmallCaps, false);
    ASSERT_THAT(direct.glyphs, SizeIs(1));
    EXPECT_THAT(
        direct.glyphs[0].glyphIndex,
        testing::Eq(latinHasFeature ? native.glyphs[0].glyphIndex : upper.glyphs[0].glyphIndex));
    EXPECT_THAT(direct.glyphs[0].fontSizeScale, FloatEq(latinHasFeature ? 1.0f : 0.8f));

    TextEngine engine(fontManager, registry, std::move(backend));
    components::ComputedTextComponent text;
    auto& span = text.spans.emplace_back();
    span.text = RcString("a");
    span.start = 0;
    span.end = 1;
    span.startsNewChunk = true;
    span.fontVariant = FontVariant::SmallCaps;
    span.fontSynthesisSmallCaps = FontSynthesisSmallCaps::None;
    TextLayoutParams params;
    params.fontSize = Lengthd(64.0, Lengthd::Unit::Px);
    params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
    params.fontFamilies = {RcString("CapsScriptControl")};
    const auto runs = engine.layout(text, params);
    ASSERT_THAT(runs, SizeIs(1));
    ASSERT_THAT(runs[0].glyphs, SizeIs(1));
    EXPECT_THAT(
        runs[0].glyphs[0].glyphIndex,
        testing::Eq(latinHasFeature ? native.glyphs[0].glyphIndex : lower.glyphs[0].glyphIndex));
    EXPECT_THAT(runs[0].glyphs[0].fontSizeScale, FloatEq(1.0f));
  }
}

TEST(TextBackendSimpleCapabilities, NativeSmallCapsUseTheLatinScriptFeature) {
  CheckLatinSmallCapsScript(true);
}

TEST(TextBackendSimpleCapabilities, NativeSmallCapsDoNotBorrowFromDefaultScriptForLatin) {
  CheckLatinSmallCapsScript(false);
}

TEST(TextBackendSimpleCapabilities, NativeSmallCapsUseTheSelectedLanguage) {
  Registry registry;
  FontManager manager(registry);
  TextBackendFull full(manager, registry);
  TextBackendSimple simple(manager, registry);
  const FontHandle original = LoadResvgFont(manager, "NotoSans-Regular.ttf", "CapsReference");
  const FontHandle modified =
      RegisterCapsFaceWithoutScriptFeatures(manager, 0x6C61746Eu, true);  // latn.
  ASSERT_TRUE(static_cast<bool>(original));
  ASSERT_TRUE(static_cast<bool>(modified));
  const auto expected =
      full.shapeRun(original, 64.f, "a", 0, 1, false, FontVariant::SmallCaps, false);
  ASSERT_THAT(expected.glyphs, ElementsAre(GlyphFontSizeScaleIs(FloatEq(1.0f))));

  TextBackend::ShapingContext context{0, 1};
  context.openTypeLanguage = 0x43415420;  // CAT: Noto Sans has a Catalan language system.
  for (const TextBackend* backend : std::array<const TextBackend*, 2>{&simple, &full}) {
    EXPECT_FALSE(backend->hasSmallCapsFeature(modified));
    EXPECT_TRUE(backend->hasSmallCapsFeatureForRun(modified, "a", 0, 1, false,
                                                   TextOrientation::Mixed, &context));
    const auto actual =
        backend->shapeRunInContext(modified, 64.f, "a", context, 0, 1, false,
                                   TextOrientation::Mixed, FontVariant::SmallCaps, false);
    EXPECT_THAT(actual.glyphs,
                Pointwise(SameGlyphAndAdvanceAs(FullTierPrecisionPx(64.0)), expected.glyphs));
  }
}

TEST(TextBackendSimpleCapabilities, SvgSmallCapsSynthesisNoneUsesTheRunScript) {
  const auto render = [](bool reference, bool latinHasFeature, bool fullTier) {
    const std::string_view smallCaps =
        "<text x='8' y='50' font-family='CapsScriptControl' font-size='64' "
        "font-variant='small-caps' style='font-synthesis-small-caps:none'>a</text>";
    const std::string_view normal =
        "<text x='8' y='50' font-family='CapsScriptControl' font-size='64'>a</text>";
    auto document =
        instantiateSubtree(reference && !latinHasFeature ? normal : smallCaps, {}, {80, 64});
    auto& registry = document.registry();
    auto& manager = registry.ctx().emplace<FontManager>(registry);
    const FontHandle font =
        reference ? LoadResvgFont(manager, "NotoSans-Regular.ttf", "CapsScriptControl")
                  : RegisterCapsFaceWithoutScriptFeatures(
                        manager, latinHasFeature ? 0x44464C54u : 0x6C61746Eu);
    if (!font) {
      return RendererBitmap{};
    }
    std::unique_ptr<TextBackend> backend;
    if (fullTier) {
      backend = std::make_unique<TextBackendFull>(manager, registry);
    } else {
      backend = std::make_unique<TextBackendSimple>(manager, registry);
    }
    registry.ctx().emplace<TextEngine>(manager, registry, std::move(backend));
    return RenderDocumentWithActiveBackend(document);
  };
  for (bool fullTier : {false, true}) {
    for (bool latinHasFeature : {false, true}) {
      SCOPED_TRACE(testing::Message() << "full=" << fullTier << " latn=" << latinHasFeature);
      const auto expected = render(true, latinHasFeature, fullTier);
      const auto actual = render(false, latinHasFeature, fullTier);
      ASSERT_THAT(expected.dimensions, testing::Eq(Vector2i(80, 64)));
      ASSERT_THAT(actual.dimensions, testing::Eq(expected.dimensions));
      ASSERT_THAT(expected.pixels, Not(Each(testing::Eq(0))));
      const auto label = std::string("small_caps_script_") + (fullTier ? "full_" : "simple_") +
                         (latinHasFeature ? "latn" : "default");
      editor::tests::CompareBitmapToBitmap(actual, expected, label,
                                           editor::tests::PixelmatchIdentityParams());
    }
  }
}

// The simple backend reads only trusted faces, so the GSUB reader never sees document-supplied
// bytes: an untrusted load of a face with smcp reports no feature and shapes nothing.
TEST(TextBackendSimpleCapabilities, UntrustedFaceHasNoSmallCapsFeature) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendSimple backend(fontManager, registry);
  const FontHandle font = fontManager.loadFontData(ReadResvgFontBytes("NotoSans-Regular.ttf"),
                                                   FontDataTrust::Untrusted);
  ASSERT_TRUE(static_cast<bool>(font));
  EXPECT_FALSE(backend.hasSmallCapsFeature(font));
  EXPECT_THAT(backend.shapeRun(font, 64.0f, "a", 0, 1, false, FontVariant::SmallCaps, false).glyphs,
              IsEmpty());
}

// Glyph outlines at a fractional size are the font's outlines scaled linearly, as the simple
// backend computes them in floating point, and so are outlines too large to oversample.
TEST(TextBackendFullCapabilities, FractionalSizeOutlinesKeepFontUnitPrecision) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendSimple simpleBackend(fontManager, registry);
  TextBackendFull fullBackend(fontManager, registry);

  const FontHandle font = LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Noto Sans");
  ASSERT_TRUE(static_cast<bool>(font));
  const auto shaped =
      simpleBackend.shapeRunNoKerning(font, 16.0f, "Hgo", 0, 3, false, FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, SizeIs(3));
  for (float size : {14.3f, 3000.3f, 4500.7f}) {
    const float scale = simpleBackend.scaleForEmToPixels(font, size);
    for (const auto& glyph : shaped.glyphs) {
      SCOPED_TRACE(testing::Message() << "glyph " << glyph.glyphIndex << " at " << size << " px");
      const Box2d expected = simpleBackend.glyphOutline(font, glyph.glyphIndex, scale).bounds();
      ASSERT_THAT(expected.width(), Gt(1.0));
      EXPECT_THAT(fullBackend.glyphOutline(font, glyph.glyphIndex, scale).bounds(),
                  BoxNear(expected, FullTierPrecisionPx(size)));
    }
  }
}

TEST(TextBackendFullCapabilities, BitmapGlyphReturnsEmojiBitmap) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull backend(fontManager, registry);

  const FontHandle font = LoadResvgFont(fontManager, "NotoColorEmoji.ttf", "Noto Color Emoji");
  ASSERT_TRUE(static_cast<bool>(font));

  const auto shaped =
      backend.shapeRun(font, 32.0f, "\xF0\x9F\x98\x81", 0, 4, false, FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, ElementsAre(GlyphIndexIs(Gt(0))));

  const float scale = backend.scaleForEmToPixels(font, 32.0f);
  const auto bitmap = backend.bitmapGlyph(font, shaped.glyphs[0].glyphIndex, scale);
  ASSERT_TRUE(bitmap.has_value());
  EXPECT_GT(bitmap->width, 0);
  EXPECT_GT(bitmap->height, 0);
  EXPECT_THAT(bitmap->rgbaPixels, SizeIs(static_cast<size_t>(bitmap->width * bitmap->height * 4)));
  EXPECT_GT(bitmap->scale, 0.0);
}

TEST(TextBackendFullCapabilities, UntrustedBitmapFontIsRejectedBeforeFreeTypeShaping) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull backend(fontManager, registry);

  const FontHandle font =
      LoadResvgFont(fontManager, "NotoColorEmoji.ttf", "Untrusted Color Emoji", false);
  ASSERT_TRUE(static_cast<bool>(font));
  EXPECT_FALSE(fontManager.isTrustedFont(font));
  EXPECT_FALSE(backend.isBitmapOnly(font));

  constexpr std::string_view kEmoji = "\xF0\x9F\x98\x81";
  EXPECT_THAT(
      backend.shapeRun(font, 32.0f, kEmoji, 0, kEmoji.size(), false, FontVariant::Normal, false)
          .glyphs,
      IsEmpty());
  EXPECT_FALSE(backend.bitmapGlyph(font, 1, 1.0f).has_value());
}

TEST(TextBackendFullCapabilities, MissingXHeightRespectsUntrustedOutlineAdmission) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull backend(fontManager, registry);
  const auto fallback = fontManager.fallbackFont();
  std::vector<uint8_t> bytes = AddEmptyGlyfTable(fontManager.fontData(fallback));
  ASSERT_GE(bytes.size(), 12u);
  for (size_t i = 0; i < ReadBe16(bytes, 4); ++i) {
    const size_t entry = 12 + i * 16;
    ASSERT_LE(entry + 16, bytes.size());
    if (ReadBe32(bytes, entry) == 0x4F532F32) {
      const size_t offset = ReadBe32(bytes, entry + 8);
      ASSERT_LE(offset + 88, bytes.size());
      WriteBe16(&bytes, offset, 1);
      WriteBe16(&bytes, offset + 86, 0);
    }
  }
  const auto font = fontManager.loadFontData(bytes, FontDataTrust::Untrusted);
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  const auto shaped = backend.shapeRun(font, 20.0f, "x", 0, 1, false, FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, ElementsAre(GlyphIndexIs(Gt(0))));
  EXPECT_THAT(fontManager.glyphOutlineComplexity(font, shaped.glyphs.front().glyphIndex),
              testing::Eq(std::nullopt));
  EXPECT_EQ(backend.fontVMetrics(font).xHeight, 0);
}

TEST(TextBackendFullCapabilities, UntrustedOutlineDecodeRequiresValidatedComplexity) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull backend(fontManager, registry);

  const FontHandle fallback = fontManager.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(fallback));
  const std::vector<uint8_t> data = AddEmptyGlyfTable(fontManager.fontData(fallback));
  ASSERT_FALSE(data.empty());
  const FontHandle untrusted = fontManager.loadFontData(data);
  ASSERT_TRUE(static_cast<bool>(untrusted));
  EXPECT_FALSE(fontManager.isTrustedFont(untrusted));
  EXPECT_FALSE(fontManager.glyphOutlineComplexity(untrusted, 1).has_value());

  const auto shaped =
      backend.shapeRun(untrusted, 32.0f, "O", 0, 1, false, FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, ElementsAre(GlyphIndexIs(Gt(0))));
  const float scale = backend.scaleForEmToPixels(untrusted, 32.0f);
  EXPECT_TRUE(backend.glyphOutline(untrusted, shaped.glyphs.front().glyphIndex, scale).empty());
}

TEST(TextBackendFullCapabilities, UntrustedOutlineFontDisablesEmbeddedBitmapLoading) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull backend(fontManager, registry);

  const FontHandle untrusted =
      LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Untrusted Noto Sans", false);
  ASSERT_TRUE(static_cast<bool>(untrusted));
  EXPECT_FALSE(fontManager.isTrustedFont(untrusted));
  EXPECT_TRUE(backend.embeddedBitmapLoadingDisabledForTesting(untrusted));

  const FontHandle trusted =
      LoadResvgFont(fontManager, "NotoSans-Regular.ttf", "Trusted Noto Sans");
  ASSERT_TRUE(static_cast<bool>(trusted));
  EXPECT_FALSE(backend.embeddedBitmapLoadingDisabledForTesting(trusted));
}

TEST(TextBackendFullCapabilities, UntrustedMixedOutlineBitmapFontDisablesBitmapLoading) {
  Registry registry;
  FontManager fontManager(registry);
  TextBackendFull backend(fontManager, registry);

  const std::vector<uint8_t> outlineFont = ReadResvgFontBytes("NotoSans-Regular.ttf");
  ASSERT_FALSE(outlineFont.empty());
  const std::vector<uint8_t> mixedFont = AddEmptyBitmapTables(outlineFont);
  ASSERT_FALSE(mixedFont.empty());
  const FontHandle font = fontManager.loadFontData(mixedFont, FontDataTrust::Untrusted);
  ASSERT_TRUE(static_cast<bool>(font));
  ASSERT_TRUE(fontManager.sfntTable(font, "glyf").has_value());
  ASSERT_TRUE(fontManager.sfntTable(font, "CBDT").has_value());
  ASSERT_TRUE(fontManager.sfntTable(font, "CBLC").has_value());
  EXPECT_TRUE(backend.embeddedBitmapLoadingDisabledForTesting(font));
  EXPECT_THAT(backend.shapeRun(font, 16.0f, "A", 0, 1, false, FontVariant::Normal, false).glyphs,
              Not(IsEmpty()));
}

}  // namespace

}  // namespace donner::svg
