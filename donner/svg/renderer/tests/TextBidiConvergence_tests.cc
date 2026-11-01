/// @file
/// Independent outline controls for ordinary mixed-direction text.
#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/css/FontFace.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/tests/ParserTestUtils.h"
#include "donner/svg/text/TextBackendFull.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg {
namespace {

class TextBidiConvergence : public testing::Test {
protected:
  struct PaintedOutline {
    Path path;
    std::string fill;
    unsigned paintGroup;
  };

  TextBidiConvergence() : fonts_(registry_), backend_(fonts_, registry_) {}

  void SetUp() override {
    const std::string path =
        Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts/Amiri-Regular.ttf");
    std::ifstream file(path, std::ios::binary);
    ASSERT_THAT(file.is_open(), testing::IsTrue());
    bytes_ = std::make_shared<const std::vector<uint8_t>>(std::istreambuf_iterator<char>(file),
                                                          std::istreambuf_iterator<char>());
    addFont(fonts_);
    font_ = fonts_.findFont("Bidi Control");
    ASSERT_THAT(static_cast<bool>(font_), testing::IsTrue());
  }

  std::shared_ptr<const std::vector<uint8_t>> readFont(std::string_view filename) const {
    const std::string path = Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts/" +
                                                            std::string(filename));
    std::ifstream file(path, std::ios::binary);
    EXPECT_THAT(file.is_open(), testing::IsTrue());
    return std::make_shared<const std::vector<uint8_t>>(std::istreambuf_iterator<char>(file),
                                                        std::istreambuf_iterator<char>());
  }

  void expectHebrewFontSupport() const {
    Registry registry;
    FontManager fonts(registry);
    addFont(fonts, readFont("MPLUS1p-Regular.ttf"));
    const auto font = fonts.findFont("Bidi Control");
    ASSERT_THAT(static_cast<bool>(font), testing::IsTrue());
    TextBackendFull backend(fonts, registry);
    const std::string text = "אבג";
    const auto shaped =
        backend.shapeRun(font, 32, text, 0, text.size(), false, FontVariant::Normal, false);
    ASSERT_THAT(shaped.glyphs, testing::SizeIs(3));
    EXPECT_THAT(shaped.glyphs,
                testing::Each(testing::Field("glyphIndex", &TextBackend::ShapedGlyph::glyphIndex,
                                             testing::Gt(0))));
  }

  void addFont(FontManager& fonts,
               std::shared_ptr<const std::vector<uint8_t>> data = nullptr) const {
    css::FontFaceSource source;
    source.kind = css::FontFaceSource::Kind::Data;
    source.payload = data ? std::move(data) : bytes_;
    source.trusted = true;
    css::FontFace face;
    face.familyName = RcString("Bidi Control");
    face.sources.push_back(std::move(source));
    fonts.addFontFace(face);
  }

  RendererBitmap renderText(std::string_view text, double letterSpacing = 0,
                            std::string_view fontFile = "Amiri-Regular.ttf") const {
    auto document = instantiateSubtree(
        "<text x='20' y='64' font-family='Bidi Control' font-size='32' font-kerning='none' "
        "letter-spacing='" +
            std::to_string(letterSpacing) + "'>" + std::string(text) + "</text>",
        {}, {260, 100});
    auto& registry = document.registry();
    auto& fonts = registry.ctx().emplace<FontManager>(registry);
    addFont(fonts, fontFile == "Amiri-Regular.ttf" ? bytes_ : readFont(fontFile));
    registry.ctx().emplace<TextEngine>(fonts, registry,
                                       std::make_unique<TextBackendFull>(fonts, registry));
    return RenderDocumentWithActiveBackend(document);
  }

  // Every input is one known script. This oracle does not ask the layout engine to order text.
  void appendRun(std::string_view text, double letterSpacing = 0, bool splitPaint = false) {
    const auto shaped = backend_.shapeRunNoKerning(font_, 32, text, 0, text.size(), false,
                                                   FontVariant::Normal, false);
    ASSERT_THAT(shaped.glyphs, testing::Not(testing::IsEmpty()));
    const float scale = backend_.scaleForEmToPixels(font_, 32);
    for (const auto& glyph : shaped.glyphs) {
      EXPECT_THAT(glyph.glyphIndex, testing::Gt(0));
      Path outline = backend_.glyphOutline(font_, glyph.glyphIndex, scale);
      if (!outline.empty()) {
        const Transform2d textFromGlyph =
            Transform2d::Translate(penX_ + glyph.xOffset, 64 + glyph.yOffset);
        const unsigned group = splitPaint && glyph.cluster >= 4 ? 1u : 0u;
        outlines_.push_back({outline.transformed(textFromGlyph),
                             splitPaint ? (group == 0 ? "red" : "blue") : "black", group});
      }
      penX_ += glyph.xAdvance + letterSpacing;
    }
  }

  RendererBitmap renderOutlines() {
    // Paint follows source spans even when their glyphs occur in the opposite visual order.
    std::stable_sort(outlines_.begin(), outlines_.end(),
                     [](const PaintedOutline& a, const PaintedOutline& b) {
                       return a.paintGroup < b.paintGroup;
                     });
    std::string source;
    for (size_t index = 0; index < outlines_.size(); ++index) {
      source +=
          "<path id='glyph" + std::to_string(index) + "' fill='" + outlines_[index].fill + "'/>";
    }
    auto document = instantiateSubtree(source, {}, {260, 100});
    for (size_t index = 0; index < outlines_.size(); ++index) {
      auto element = document.querySelector("#glyph" + std::to_string(index));
      EXPECT_THAT(element.has_value(), testing::IsTrue());
      if (element) {
        element->cast<SVGPathElement>().setSpline(outlines_[index].path);
      }
    }
    return RenderDocumentWithActiveBackend(document);
  }

  RendererBitmap renderEmpty() const {
    auto document = instantiateSubtree("<g/>", {}, {260, 100});
    return RenderDocumentWithActiveBackend(document);
  }

private:
  Registry registry_;
  FontManager fonts_;
  TextBackendFull backend_;
  FontHandle font_;
  std::shared_ptr<const std::vector<uint8_t>> bytes_;
  std::vector<PaintedOutline> outlines_;
  double penX_ = 20;
};

TEST_F(TextBidiConvergence, LatinOutlineControlMatchesText) {
  appendRun("AB.");
  const auto actual = renderText("AB.");
  const auto expected = renderOutlines();
  const auto empty = renderEmpty();
  SCOPED_TRACE(ActiveRendererBackendName());
  ASSERT_THAT(actual.dimensions, testing::Eq(Vector2i(260, 100)));
  ASSERT_THAT(expected.pixels, testing::Not(testing::IsEmpty()));
  EXPECT_NONFATAL_FAILURE(
      editor::tests::CompareBitmapToBitmap(expected, empty, "bidi_latin_outline_control_visible",
                                           editor::tests::PixelmatchIdentityParams()),
      "pixels differ");
  editor::tests::CompareBitmapToBitmap(actual, expected, "bidi_latin_outline_control",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(TextBidiConvergence, LatinArabicAndPunctuationKeepTheirOwnDirectionsWithSpacing) {
  appendRun("AB ", 5);
  appendRun("مر");
  appendRun(".", 5);
  const auto actual = renderText("AB مر.", 5);
  const auto expected = renderOutlines();
  const auto empty = renderEmpty();
  SCOPED_TRACE(ActiveRendererBackendName());
  ASSERT_THAT(actual.dimensions, testing::Eq(Vector2i(260, 100)));
  ASSERT_THAT(expected.pixels, testing::Not(testing::IsEmpty()));
  EXPECT_NONFATAL_FAILURE(
      editor::tests::CompareBitmapToBitmap(expected, empty, "bidi_mixed_scripts_spacing_visible",
                                           editor::tests::PixelmatchIdentityParams()),
      "pixels differ");
  editor::tests::CompareBitmapToBitmap(actual, expected, "bidi_mixed_scripts_spacing",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(TextBidiConvergence, ArabicFirstKeepsLatinAndPunctuationInLeftToRightParagraphOrder) {
  appendRun("مر");
  appendRun(" AB.");
  const auto actual = renderText("مر AB.");
  const auto expected = renderOutlines();
  const auto empty = renderEmpty();
  SCOPED_TRACE(ActiveRendererBackendName());
  ASSERT_THAT(actual.dimensions, testing::Eq(Vector2i(260, 100)));
  ASSERT_THAT(expected.pixels, testing::Not(testing::IsEmpty()));
  EXPECT_NONFATAL_FAILURE(
      editor::tests::CompareBitmapToBitmap(expected, empty, "bidi_arabic_first_paragraph_visible",
                                           editor::tests::PixelmatchIdentityParams()),
      "pixels differ");
  editor::tests::CompareBitmapToBitmap(actual, expected, "bidi_arabic_first_paragraph",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(TextBidiConvergence, ArabicJoiningAcrossPaintSpansPreservesSourceColors) {
  appendRun("مرحبا", 0, true);
  const auto actual = renderText("<tspan fill='red'>مر</tspan><tspan fill='blue'>حبا</tspan>");
  const auto expected = renderOutlines();
  const auto empty = renderEmpty();
  SCOPED_TRACE(ActiveRendererBackendName());
  ASSERT_THAT(actual.dimensions, testing::Eq(Vector2i(260, 100)));
  ASSERT_THAT(expected.pixels, testing::Not(testing::IsEmpty()));
  EXPECT_NONFATAL_FAILURE(
      editor::tests::CompareBitmapToBitmap(expected, empty, "bidi_arabic_span_paint_visible",
                                           editor::tests::PixelmatchIdentityParams()),
      "pixels differ");
  editor::tests::CompareBitmapToBitmap(actual, expected, "bidi_arabic_span_paint",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(TextBidiConvergence, PositionOnJoinerDoesNotDiscardFollowingVisibleGlyphs) {
  expectHebrewFontSupport();
  const auto actual =
      renderText("<tspan x='20 60 120'>אב\u200dג</tspan>", 0, "MPLUS1p-Regular.ttf");
  const auto expected = renderText("<tspan x='20 60'>אב\u200dג</tspan>", 0, "MPLUS1p-Regular.ttf");
  const auto empty = renderEmpty();
  SCOPED_TRACE(ActiveRendererBackendName());
  EXPECT_NONFATAL_FAILURE(
      editor::tests::CompareBitmapToBitmap(expected, empty, "bidi_joiner_expected_visible",
                                           editor::tests::PixelmatchIdentityParams()),
      "pixels differ");
  editor::tests::CompareBitmapToBitmap(actual, expected, "bidi_joiner_position_boundary",
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
