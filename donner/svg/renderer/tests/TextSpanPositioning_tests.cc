#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

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

// Keep the default text backend aligned with CSS Text 3 section 7.3: paint-only inline style
// changes must not break shaping across the tspan boundary.
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
  for (size_t glyphIndex = 0; glyphIndex < 4; ++glyphIndex) {
    const TextGlyph& unsplitGlyph = unsplitRuns[0].glyphs[glyphIndex];
    const TextGlyph& splitGlyph =
        glyphIndex < 2 ? splitRuns[0].glyphs[glyphIndex] : splitRuns[1].glyphs[glyphIndex - 2];
    EXPECT_DOUBLE_EQ(splitGlyph.xPosition, unsplitGlyph.xPosition) << glyphIndex;
    EXPECT_DOUBLE_EQ(splitGlyph.yPosition, unsplitGlyph.yPosition) << glyphIndex;
  }
}

}  // namespace
}  // namespace donner::svg
