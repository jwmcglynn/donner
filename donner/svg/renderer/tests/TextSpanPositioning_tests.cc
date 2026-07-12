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

FontHandle LoadNotoSans(FontManager& fontManager) {
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

// This exact Latin kerning case keeps shaping continuous across a paint-only tspan boundary.
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

}  // namespace
}  // namespace donner::svg
