#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/css/FontFace.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg {
namespace {

using ::testing::_;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::Optional;
using ::testing::SizeIs;

std::vector<uint8_t> ReadSynthesisFont(std::string_view name) {
  const std::string path =
      Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts/" + std::string(name));
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// Supplies one real upright face for every request unless the test enables its native italic.
class SynthesisStyleProvider final : public FontFamilyProvider {
public:
  std::vector<FontFamilyInfo> families() const override { return {{"sans-serif"}}; }

  bool hasFamily(std::string_view family) const override { return family == "sans-serif"; }

  std::vector<uint8_t> loadFamilyData(std::string_view family,
                                      const FontFaceRequest& request) const override {
    if (!hasFamily(family)) {
      return {};
    }
    return nativeItalic && request.style != FontStyle::Normal ? italic : regular;
  }

  std::vector<uint8_t> regular = ReadSynthesisFont("NotoSans-Regular.ttf");
  std::vector<uint8_t> italic = ReadSynthesisFont("NotoSans-Italic.ttf");
  bool nativeItalic = false;
};

struct SynthesisObservation {
  FontHandle font;
  double advance = 0.0;
  Path outline;
};

class FontSynthesisStyleTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_THAT(provider_.regular, Not(IsEmpty()));
    ASSERT_THAT(provider_.italic, Not(IsEmpty()));
    auto parsed = parser::SVGParser::ParseSVG(R"(
      <svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
        <g font-family="sans-serif" font-size="64">
          <text id="regular">A</text>
          <text id="auto" font-style="italic">A</text>
          <text id="none" font-style="italic" style="font-synthesis-style:none">A</text>
          <text id="only-italic" font-style="italic" style="font-synthesis-style:oblique-only">A</text>
          <text id="only-oblique" font-style="oblique" style="font-synthesis-style:oblique-only">A</text>
          <text id="document" font-family="DocumentFace" font-style="italic">A</text>
        </g>
      </svg>)");
    ASSERT_THAT(parsed, ParseResultIs(_));
    document_.emplace(std::move(parsed.result()));
    auto& registry = document_->registry();
    auto& fonts = registry.ctx().emplace<FontManager>(registry);
    fonts.setFontProvider(&provider_);
    registry.ctx().emplace<TextEngine>(fonts, registry);
  }

  FontManager& fonts() { return document_->registry().ctx().get<FontManager>(); }

  SynthesisObservation observe(std::string_view id) {
    const auto element = document_->querySelector("#" + std::string(id));
    EXPECT_THAT(element, Optional(_));
    if (!element) {
      return {};
    }
    auto& engine = document_->registry().ctx().get<TextEngine>();
    ParseWarningSink warnings = ParseWarningSink::Disabled();
    engine.prepareForElement(element->entityHandle(), warnings);
    const auto& geometry = engine.ensureComputedTextGeometryComponent(element->entityHandle());
    EXPECT_THAT(geometry.runs, SizeIs(1));
    EXPECT_THAT(geometry.glyphs, SizeIs(1));
    if (geometry.runs.empty() || geometry.glyphs.empty() || geometry.runs.front().glyphs.empty()) {
      return {};
    }
    return {geometry.runs.front().font, geometry.runs.front().glyphs.front().xAdvance,
            geometry.glyphs.front().path};
  }

  void setPolicy(std::string_view id, std::string_view value) {
    auto element = document_->querySelector("#" + std::string(id));
    ASSERT_THAT(element, Optional(_));
    element->setAttribute("style", "font-synthesis-style:" + std::string(value));
  }

  SynthesisStyleProvider provider_;
  std::optional<SVGDocument> document_;
};

TEST_F(FontSynthesisStyleTest, NoneSuppressesProviderObliqueWithoutChangingFaceOrAdvance) {
  const auto regular = observe("regular");
  const auto automatic = observe("auto");
  const auto disabled = observe("none");
  ASSERT_THAT(regular.outline.points(), Not(IsEmpty()));
  EXPECT_THAT(automatic.outline.points(), Not(ElementsAreArray(regular.outline.points())));
  EXPECT_THAT(disabled.font, Eq(automatic.font));
  EXPECT_THAT(disabled.advance, Eq(automatic.advance));
  EXPECT_THAT(disabled.outline.points(), ElementsAreArray(regular.outline.points()));
  EXPECT_THAT(fonts().fontData(disabled.font), ElementsAreArray(provider_.regular));
}

TEST_F(FontSynthesisStyleTest, ObliqueOnlyDistinguishesItalicFromObliqueRequests) {
  const auto regular = observe("regular");
  const auto automatic = observe("auto");
  const auto italicOnly = observe("only-italic");
  const auto obliqueOnly = observe("only-oblique");
  ASSERT_THAT(automatic.outline.points(), Not(IsEmpty()));
  EXPECT_THAT(italicOnly.outline.points(), ElementsAreArray(regular.outline.points()));
  EXPECT_THAT(obliqueOnly.outline.points(), ElementsAreArray(automatic.outline.points()));
  EXPECT_THAT(italicOnly.advance, Eq(obliqueOnly.advance));
}

TEST_F(FontSynthesisStyleTest, OppositePoliciesAndMutationPreserveCachedSiblingOutline) {
  const auto automatic = observe("auto");
  const auto disabled = observe("none");
  EXPECT_THAT(disabled.font, Eq(automatic.font));
  EXPECT_THAT(disabled.outline.points(), Not(ElementsAreArray(automatic.outline.points())));
  EXPECT_THAT(observe("auto").outline.points(), ElementsAreArray(automatic.outline.points()));
  setPolicy("none", "auto");
  EXPECT_THAT(observe("none").outline.points(), ElementsAreArray(automatic.outline.points()));
  setPolicy("none", "none");
  EXPECT_THAT(observe("none").outline.points(),
              ElementsAreArray(observe("regular").outline.points()));
  EXPECT_THAT(observe("auto").outline.points(), ElementsAreArray(automatic.outline.points()));
}

TEST_F(FontSynthesisStyleTest, NativeItalicFaceIsNeverDisabledOrReplaced) {
  provider_.nativeItalic = true;
  const auto regular = observe("regular");
  const auto automatic = observe("auto");
  const auto disabled = observe("none");
  const auto italicOnly = observe("only-italic");
  EXPECT_THAT(fonts().fontData(automatic.font), ElementsAreArray(provider_.italic));
  ASSERT_THAT(automatic.outline.points(), Not(IsEmpty()));
  EXPECT_THAT(automatic.outline.points(), Not(ElementsAreArray(regular.outline.points())));
  EXPECT_THAT(disabled.outline.points(), ElementsAreArray(automatic.outline.points()));
  EXPECT_THAT(italicOnly.outline.points(), ElementsAreArray(automatic.outline.points()));
}

TEST_F(FontSynthesisStyleTest, ExistingDocumentFaceWithoutSynthesisKeepsItsGeometry) {
  css::FontFace face;
  face.familyName = "DocumentFace";
  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Data;
  source.payload = std::make_shared<const std::vector<uint8_t>>(provider_.regular);
  source.trusted = true;
  face.sources.push_back(std::move(source));
  fonts().addFontFace(face);
  const auto original = observe("document");
  EXPECT_THAT(fonts().providerFaceRequest(original.font), Eq(std::nullopt));
  EXPECT_THAT(original.outline.points(), ElementsAreArray(observe("regular").outline.points()));
  setPolicy("document", "none");
  EXPECT_THAT(observe("document").outline.points(), ElementsAreArray(original.outline.points()));
}

}  // namespace
}  // namespace donner::svg
