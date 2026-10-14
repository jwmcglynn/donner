#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/css/FontFace.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextBackendSimple.h"
#include "donner/svg/text/TextEngine.h"
#include "donner/svg/text/TextLayoutParams.h"

namespace donner::svg {
namespace {

using ::testing::ElementsAre;
using ::testing::SizeIs;

FontHandle LoadNotoSans(FontManager& fontManager, bool trusted = true) {
  const std::string fontPath =
      Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts/NotoSans-Regular.ttf");
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
  face.familyName = RcString("Noto Sans");
  face.sources.push_back(std::move(source));
  fontManager.addFontFace(face);
  return fontManager.findFont(RcString("Noto Sans"));
}

components::ComputedTextComponent::TextSpan MakeSpan(std::string_view text, bool startsNewChunk) {
  components::ComputedTextComponent::TextSpan span;
  span.text = RcString(text);
  span.start = 0;
  span.end = text.size();
  span.startsNewChunk = startsNewChunk;
  return span;
}

TextLayoutParams MakeParams() {
  TextLayoutParams params;
  params.fontSize = Lengthd(64.0, Lengthd::Unit::Px);
  params.fontFamilies = {RcString("Noto Sans")};
  params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
  params.fontMetrics = FontMetrics();
  return params;
}

TEST(TextSpanPositioningTest, SizeAdjustUsesTheSelectedNotoSansFaceMetrics) {
  Registry registry;
  FontManager fontManager(registry);
  auto backend = std::make_unique<TextBackendSimple>(fontManager, registry);
  TextEngine engine(fontManager, registry, std::move(backend));
  const FontHandle font = LoadNotoSans(fontManager);
  ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
  EXPECT_EQ(engine.fontVMetrics(font).xHeight, 536);
  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("Text", true));
  auto params = MakeParams();
  params.fontSizeAdjust = 0.3;
  const auto runs = engine.layout(text, params);
  ASSERT_THAT(runs, ElementsAre(testing::Field(&TextRun::glyphs, SizeIs(4))));
  EXPECT_EQ(runs.front().font, font);
  EXPECT_FLOAT_EQ(runs.front().usedFontSizePx, 64.0f * 0.3f / 0.536f);
}

TEST(TextSpanPositioningTest, SimpleBackendFallsBackFromUntrustedDocumentFont) {
  Registry registry;
  FontManager fontManager(registry);
  auto backend = std::make_unique<TextBackendSimple>(fontManager, registry);
  TextEngine engine(fontManager, registry, std::move(backend));

  const FontHandle documentFont = LoadNotoSans(fontManager, false);
  ASSERT_TRUE(static_cast<bool>(documentFont));
  ASSERT_FALSE(fontManager.isTrustedFont(documentFont));

  const FontHandle fallback = fontManager.fallbackFont();
  ASSERT_TRUE(static_cast<bool>(fallback));
  ASSERT_TRUE(fontManager.isTrustedFont(fallback));

  components::ComputedTextComponent text;
  text.spans.push_back(MakeSpan("A", true));

  const auto runs = engine.layout(text, MakeParams());

  ASSERT_THAT(runs, ElementsAre(testing::Field(&TextRun::glyphs, SizeIs(1))));
  EXPECT_EQ(runs[0].font, fallback);
  const TextGlyph& glyph = runs[0].glyphs.front();
  const Path outline = engine.glyphOutline(runs[0].font, glyph.glyphIndex,
                                           engine.scaleForEmToPixels(runs[0].font, 64.0f));
  EXPECT_THAT(outline.commands(), testing::Not(testing::IsEmpty()));
}

// This exact Latin case preserves the e-x kerning result across a paint-only tspan boundary.
TEST(TextSpanPositioningTest, SimpleBackendPaintOnlySplitMatchesUnsplitGlyphPositions) {
  Registry registry;
  FontManager fontManager(registry);
  auto backend = std::make_unique<TextBackendSimple>(fontManager, registry);
  TextEngine engine(fontManager, registry, std::move(backend));
  ASSERT_TRUE(static_cast<bool>(LoadNotoSans(fontManager)));

  components::ComputedTextComponent unsplitText;
  unsplitText.spans.push_back(MakeSpan("Text", true));

  components::ComputedTextComponent splitText;
  splitText.spans.push_back(MakeSpan("Te", true));
  auto trailingSpan = MakeSpan("xt", false);
  trailingSpan.paintOrder.order = {PaintComponent::Stroke, PaintComponent::Fill,
                                   PaintComponent::Markers};
  splitText.spans.push_back(std::move(trailingSpan));

  const auto unsplitRuns = engine.layout(unsplitText, MakeParams());
  const auto splitRuns = engine.layout(splitText, MakeParams());

  ASSERT_THAT(unsplitRuns, ElementsAre(testing::Field(&TextRun::glyphs, SizeIs(4))));
  ASSERT_THAT(splitRuns, ElementsAre(testing::Field(&TextRun::glyphs, SizeIs(2)),
                                     testing::Field(&TextRun::glyphs, SizeIs(2))));
  const auto splitGlyphAt = [&splitRuns](size_t index) -> const TextGlyph& {
    return index < 2 ? splitRuns[0].glyphs[index] : splitRuns[1].glyphs[index - 2];
  };
  for (size_t glyphIndex = 0; glyphIndex < 4; ++glyphIndex) {
    const TextGlyph& unsplitGlyph = unsplitRuns[0].glyphs[glyphIndex];
    const TextGlyph& splitGlyph = splitGlyphAt(glyphIndex);
    const Path unsplitOutline =
        engine.glyphOutline(unsplitRuns[0].font, unsplitGlyph.glyphIndex,
                            engine.scaleForEmToPixels(unsplitRuns[0].font, 64.0f));
    const Path splitOutline = engine.glyphOutline(
        splitRuns[glyphIndex < 2 ? 0 : 1].font, splitGlyph.glyphIndex,
        engine.scaleForEmToPixels(splitRuns[glyphIndex < 2 ? 0 : 1].font, 64.0f));
    ASSERT_THAT(unsplitOutline.commands(), testing::Not(testing::IsEmpty()));
    ASSERT_THAT(splitOutline.commands(), testing::Not(testing::IsEmpty()));
    const Box2d unsplitBounds = unsplitOutline.transformedBounds(
        Transform2d::Translate(Vector2d(unsplitGlyph.xPosition, unsplitGlyph.yPosition)));
    const Box2d splitBounds = splitOutline.transformedBounds(
        Transform2d::Translate(Vector2d(splitGlyph.xPosition, splitGlyph.yPosition)));

    EXPECT_EQ(splitGlyph.glyphIndex, unsplitGlyph.glyphIndex) << glyphIndex;
    EXPECT_DOUBLE_EQ(splitGlyph.xPosition, unsplitGlyph.xPosition) << glyphIndex;
    EXPECT_DOUBLE_EQ(splitGlyph.yPosition, unsplitGlyph.yPosition) << glyphIndex;
    EXPECT_DOUBLE_EQ(splitGlyph.yAdvance, unsplitGlyph.yAdvance) << glyphIndex;
    // Compare the placed advance so cross-span kerning has the same meaning regardless of
    // whether the backend folds it into xAdvance or applies it at the next glyph position.
    const double unsplitAdvance =
        glyphIndex + 1 < 4
            ? unsplitRuns[0].glyphs[glyphIndex + 1].xPosition - unsplitGlyph.xPosition
            : unsplitGlyph.xAdvance;
    const double splitAdvance = glyphIndex + 1 < 4
                                    ? splitGlyphAt(glyphIndex + 1).xPosition - splitGlyph.xPosition
                                    : splitGlyph.xAdvance;
    EXPECT_DOUBLE_EQ(splitAdvance, unsplitAdvance) << glyphIndex;
    EXPECT_THAT(splitBounds,
                BoxEq(Vector2Eq(testing::DoubleNear(unsplitBounds.topLeft.x, 1e-9),
                                testing::DoubleNear(unsplitBounds.topLeft.y, 1e-9)),
                      Vector2Eq(testing::DoubleNear(unsplitBounds.bottomRight.x, 1e-9),
                                testing::DoubleNear(unsplitBounds.bottomRight.y, 1e-9))))
        << glyphIndex;
  }
}

/// Holds the font-backed engine and an observer valid for the engine's lifetime.
struct TextPathPairFixture {
  Registry registry;
  FontManager fonts{registry};
  TextBackendSimple* backend = nullptr;
  std::unique_ptr<TextEngine> engine;
  FontHandle font;

  TextPathPairFixture() {
    auto ownedBackend = std::make_unique<TextBackendSimple>(fonts, registry);
    backend = ownedBackend.get();
    engine = std::make_unique<TextEngine>(fonts, registry, std::move(ownedBackend));
    font = LoadNotoSans(fonts);
  }
};

components::ComputedTextComponent::TextSpan MakeStraightPathSpan(std::string_view text,
                                                                 bool startsNewChunk,
                                                                 Entity source = Entity{1},
                                                                 double startX = 0.0) {
  auto span = MakeSpan(text, startsNewChunk);
  span.pathSpline =
      PathBuilder().moveTo(Vector2d(startX, 0.0)).lineTo(Vector2d(startX + 400.0, 0.0)).build();
  span.textPathSourceEntity = source;
  return span;
}

/// Derive straight-baseline origins from the font backend's independent whole-run metrics.
std::vector<double> StraightPathOrigins(const TextBackend::ShapedRun& shaped, double startX = 0.0) {
  std::vector<double> result;
  double advance = startX;
  for (const auto& glyph : shaped.glyphs) {
    advance += glyph.xKern;
    result.push_back(advance + glyph.xOffset);
    advance += glyph.xAdvance;
  }
  return result;
}

std::vector<TextGlyph> PathGlyphs(const std::vector<TextRun>& runs) {
  std::vector<TextGlyph> glyphs;
  for (const auto& run : runs) {
    glyphs.insert(glyphs.end(), run.glyphs.begin(), run.glyphs.end());
  }
  return glyphs;
}

TEST(TextSpanPositioningTest, SimpleBackendTextPathSplitPreservesPairKerning) {
  TextPathPairFixture fixture;
  ASSERT_TRUE(static_cast<bool>(fixture.font));
  const auto shaped = fixture.backend->shapeRun(fixture.font, 64.0f, "Text", 0, 4, false,
                                                FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, SizeIs(4));
  const double boundaryKern =
      fixture.backend->crossSpanKern(fixture.font, 64.0f, fixture.font, 64.0f, U'e', U'x', false);
  ASSERT_NE(boundaryKern, 0.0) << "The font pair must exercise an actual kerning adjustment";
  EXPECT_DOUBLE_EQ(shaped.glyphs[2].xKern, boundaryKern);
  const auto expected = StraightPathOrigins(shaped);

  components::ComputedTextComponent unsplit;
  unsplit.spans.push_back(MakeStraightPathSpan("Text", true));
  components::ComputedTextComponent split;
  split.spans.push_back(MakeStraightPathSpan("Te", true));
  auto tail = MakeStraightPathSpan("xt", false);
  tail.paintOrder.order = {PaintComponent::Stroke, PaintComponent::Fill, PaintComponent::Markers};
  split.spans.push_back(std::move(tail));
  const auto wholeGlyphs = PathGlyphs(fixture.engine->layout(unsplit, MakeParams()));
  const auto splitGlyphs = PathGlyphs(fixture.engine->layout(split, MakeParams()));
  ASSERT_THAT(wholeGlyphs, SizeIs(4));
  ASSERT_THAT(splitGlyphs, SizeIs(4));
  for (size_t i = 0; i < shaped.glyphs.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(wholeGlyphs[i].glyphIndex, shaped.glyphs[i].glyphIndex);
    EXPECT_EQ(splitGlyphs[i].glyphIndex, shaped.glyphs[i].glyphIndex);
    EXPECT_DOUBLE_EQ(wholeGlyphs[i].xPosition, expected[i]);
    EXPECT_DOUBLE_EQ(splitGlyphs[i].xPosition, expected[i]);
    EXPECT_DOUBLE_EQ(splitGlyphs[i].yPosition, shaped.glyphs[i].yOffset);
    EXPECT_DOUBLE_EQ(splitGlyphs[i].xAdvance, shaped.glyphs[i].xAdvance);
    EXPECT_DOUBLE_EQ(splitGlyphs[i].rotateDegrees, 0.0);
  }
}

TEST(TextSpanPositioningTest, SimpleBackendTextPathYOnlySplitPreservesPairKerning) {
  TextPathPairFixture fixture;
  ASSERT_TRUE(static_cast<bool>(fixture.font));
  const auto shaped = fixture.backend->shapeRun(fixture.font, 64.0f, "Text", 0, 4, false,
                                                FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, SizeIs(4));
  ASSERT_NE(
      fixture.backend->crossSpanKern(fixture.font, 64.0f, fixture.font, 64.0f, U'e', U'x', false),
      0.0);
  const auto expected = StraightPathOrigins(shaped);
  components::ComputedTextComponent text;
  text.spans.push_back(MakeStraightPathSpan("Te", true));
  auto tail = MakeStraightPathSpan("xt", true);
  tail.yList = {Lengthd(20.0, Lengthd::Unit::Px), Lengthd(40.0, Lengthd::Unit::Px)};
  ASSERT_THAT(tail.xList, testing::IsEmpty());
  text.spans.push_back(std::move(tail));
  const auto glyphs = PathGlyphs(fixture.engine->layout(text, MakeParams()));
  ASSERT_THAT(glyphs, SizeIs(4));
  for (size_t i = 0; i < shaped.glyphs.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(glyphs[i].glyphIndex, shaped.glyphs[i].glyphIndex);
    EXPECT_DOUBLE_EQ(glyphs[i].xPosition, expected[i]);
    EXPECT_DOUBLE_EQ(glyphs[i].yPosition, shaped.glyphs[i].yOffset);
    EXPECT_DOUBLE_EQ(glyphs[i].xAdvance, shaped.glyphs[i].xAdvance);
  }
}

TEST(TextSpanPositioningTest, SimpleBackendTextPathEmptySpanPreservesPairKerning) {
  TextPathPairFixture fixture;
  ASSERT_TRUE(static_cast<bool>(fixture.font));
  const auto shaped = fixture.backend->shapeRun(fixture.font, 64.0f, "Text", 0, 4, false,
                                                FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, SizeIs(4));
  ASSERT_NE(
      fixture.backend->crossSpanKern(fixture.font, 64.0f, fixture.font, 64.0f, U'e', U'x', false),
      0.0);
  const auto expected = StraightPathOrigins(shaped);
  components::ComputedTextComponent text;
  text.spans.push_back(MakeStraightPathSpan("Te", true));
  text.spans.push_back(MakeStraightPathSpan("", false));
  text.spans.push_back(MakeStraightPathSpan("xt", false));
  const auto runs = fixture.engine->layout(text, MakeParams());
  ASSERT_THAT(runs, SizeIs(3));
  EXPECT_THAT(runs[1].glyphs, SizeIs(0));
  const auto glyphs = PathGlyphs(runs);
  ASSERT_THAT(glyphs, SizeIs(4));
  for (size_t i = 0; i < shaped.glyphs.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(glyphs[i].glyphIndex, shaped.glyphs[i].glyphIndex);
    EXPECT_DOUBLE_EQ(glyphs[i].xPosition, expected[i]);
    EXPECT_DOUBLE_EQ(glyphs[i].xAdvance, shaped.glyphs[i].xAdvance);
  }
}

TEST(TextSpanPositioningTest, SimpleBackendTextPathNoKerningKeepsSplitAndUnsplitEqual) {
  TextPathPairFixture fixture;
  ASSERT_TRUE(static_cast<bool>(fixture.font));
  const auto shaped = fixture.backend->shapeRunNoKerning(fixture.font, 64.0f, "Text", 0, 4, false,
                                                         FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, SizeIs(4));
  const auto expected = StraightPathOrigins(shaped);
  components::ComputedTextComponent whole;
  whole.spans.push_back(MakeStraightPathSpan("Text", true));
  components::ComputedTextComponent split;
  split.spans.push_back(MakeStraightPathSpan("Te", true));
  split.spans.push_back(MakeStraightPathSpan("xt", false));
  auto params = MakeParams();
  params.fontKerning = FontKerning::None;
  const auto wholeGlyphs = PathGlyphs(fixture.engine->layout(whole, params));
  const auto splitGlyphs = PathGlyphs(fixture.engine->layout(split, params));
  ASSERT_THAT(wholeGlyphs, SizeIs(4));
  ASSERT_THAT(splitGlyphs, SizeIs(4));
  for (size_t i = 0; i < expected.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_DOUBLE_EQ(wholeGlyphs[i].xPosition, expected[i]);
    EXPECT_DOUBLE_EQ(splitGlyphs[i].xPosition, expected[i]);
    EXPECT_DOUBLE_EQ(splitGlyphs[i].xAdvance, shaped.glyphs[i].xAdvance);
  }
}

TEST(TextSpanPositioningTest, SimpleBackendTextPathNewPathStartsWithoutPriorPairKerning) {
  TextPathPairFixture fixture;
  ASSERT_TRUE(static_cast<bool>(fixture.font));
  const auto shaped =
      fixture.backend->shapeRun(fixture.font, 64.0f, "xt", 0, 2, false, FontVariant::Normal, false);
  ASSERT_THAT(shaped.glyphs, SizeIs(2));
  const auto expected = StraightPathOrigins(shaped, 100.0);
  components::ComputedTextComponent text;
  text.spans.push_back(MakeStraightPathSpan("Te", true, Entity{1}));
  text.spans.push_back(MakeStraightPathSpan("xt", false, Entity{2}, 100.0));
  const auto runs = fixture.engine->layout(text, MakeParams());
  ASSERT_THAT(runs, SizeIs(2));
  ASSERT_THAT(runs[1].glyphs, SizeIs(2));
  for (size_t i = 0; i < expected.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(runs[1].glyphs[i].glyphIndex, shaped.glyphs[i].glyphIndex);
    EXPECT_DOUBLE_EQ(runs[1].glyphs[i].xPosition, expected[i]);
    EXPECT_DOUBLE_EQ(runs[1].glyphs[i].xAdvance, shaped.glyphs[i].xAdvance);
  }
}

}  // namespace
}  // namespace donner::svg
