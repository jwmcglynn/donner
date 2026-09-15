/// @file
/// Face-selection coverage for the hermetic test font set.
///
/// These tests assert which font file a family request resolves to, independently of any pixel
/// comparison, so a selection regression is reported as a named face mismatch instead of a diff
/// count.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/css/FontFace.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/resources/FontMetadata.h"
#include "donner/svg/text/TextEngine.h"
#include "donner/svg/text/TextLayoutParams.h"

namespace donner::svg {
namespace {

using ::testing::Eq;
using ::testing::Field;
using ::testing::SizeIs;

/// Sentinel source name reported when a lookup resolved to the built-in Public Sans face.
constexpr const char* kEmbeddedFallback = "<embedded Public Sans fallback>";

std::filesystem::path TestFontsDir() {
  return std::filesystem::path(
      Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts"));
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  file.seekg(0, std::ios::end);
  const auto size = file.tellg();
  file.seekg(0);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(data.data()), size);
  return data;
}

/// Font files in the hermetic set, keyed by filename, so a resolved handle can be traced back to
/// the exact bytes it was loaded from.
const std::map<std::string, std::vector<uint8_t>>& VendoredFontBytes() {
  static const std::map<std::string, std::vector<uint8_t>> bytes = [] {
    std::map<std::string, std::vector<uint8_t>> result;
    for (const auto& entry : std::filesystem::directory_iterator(TestFontsDir())) {
      const std::string extension = entry.path().extension().string();
      if (extension != ".ttf" && extension != ".otf") {
        continue;
      }
      result.emplace(entry.path().filename().string(), ReadFile(entry.path()));
    }
    return result;
  }();
  return bytes;
}

/// The identity of a resolved face: where its bytes came from, plus the name-table and OS/2 values
/// that decide CSS font matching.
struct SelectedFace {
  std::string source;      ///< Vendored font filename, or \ref kEmbeddedFallback.
  std::string familyName;  ///< Family name from the font's name table.
  int weight = 0;          ///< OS/2 usWeightClass.
  int style = 0;           ///< 0 = normal, 1 = italic, 2 = oblique.

  bool operator==(const SelectedFace&) const = default;
};

std::ostream& operator<<(std::ostream& os, const SelectedFace& face) {
  return os << "SelectedFace{source=\"" << face.source << "\", family=\"" << face.familyName
            << "\", weight=" << face.weight << ", style=" << face.style << "}";
}

/// Matches the vendored font file a face was loaded from.
auto FaceFrom(std::string_view source) {
  return Field("source", &SelectedFace::source, Eq(std::string(source)));
}

SelectedFace DescribeFace(const FontManager& fontManager, FontHandle handle) {
  SelectedFace face;
  if (!handle) {
    face.source = "<no face>";
    return face;
  }

  const std::span<const uint8_t> data = fontManager.fontData(handle);
  face.source = kEmbeddedFallback;
  for (const auto& [filename, bytes] : VendoredFontBytes()) {
    if (data.size() == bytes.size() && std::equal(data.begin(), data.end(), bytes.begin())) {
      face.source = filename;
      break;
    }
  }

  if (const std::optional<FontMetadata> metadata = ParseFontMetadata(data)) {
    face.familyName = metadata->familyName;
    face.weight = metadata->fontWeight;
    face.style = metadata->fontStyle;
  }
  return face;
}

/// A FontManager carrying the same faces and generic-family mappings the renderer image
/// comparisons use.
class HermeticFontSet {
public:
  HermeticFontSet() {
    ParseWarningSink warnings = ParseWarningSink::Disabled();
    parser::SVGParser::Options options;
    auto result = parser::SVGParser::ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg"/>)",
                                              warnings, options);
    EXPECT_FALSE(result.hasError()) << "Parse error: " << result.error();
    document_ = std::move(result.result());
    RegisterFontsFromDirectoryForTesting(document_, TestFontsDir());

    // The hermetic set arrives as document resources; the text pipeline hands them to the font
    // manager on the first layout. Do the same here so face matching can be queried directly.
    auto& resourceManager = document_.registry().ctx().get<components::ResourceManagerContext>();
    for (const css::FontFace& face : resourceManager.fontFaces()) {
      fontManager().addFontFace(face);
    }
  }

  FontManager& fontManager() { return document_.registry().ctx().get<FontManager>(); }

  Registry& registry() { return document_.registry(); }

  SelectedFace select(std::string_view family) {
    return DescribeFace(fontManager(), fontManager().findFont(family));
  }

  SelectedFace select(std::string_view family, int weight, int style, int stretch) {
    return DescribeFace(fontManager(), fontManager().findFont(family, weight, style, stretch));
  }

private:
  SVGDocument document_;
};

/// Resolves the face the text pipeline actually uses for the first `<text>` element in @p svg,
/// exercising the same family-list cascade as rendering.
SelectedFace ResolveDocumentFace(std::string_view svg) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  parser::SVGParser::Options options;
  auto result = parser::SVGParser::ParseSVG(svg, warnings, options);
  EXPECT_FALSE(result.hasError()) << "Parse error: " << result.error();
  if (result.hasError()) {
    return {};
  }

  SVGDocument document = std::move(result.result());
  RegisterFontsFromDirectoryForTesting(document, TestFontsDir());

  std::optional<SVGElement> textElement = document.querySelector("text");
  EXPECT_TRUE(textElement.has_value()) << "SVG under test has no <text> element";
  if (!textElement) {
    return {};
  }

  Registry& registry = document.registry();
  auto& fontManager = registry.ctx().get<FontManager>();
  auto& textEngine = registry.ctx().contains<TextEngine>()
                         ? registry.ctx().get<TextEngine>()
                         : registry.ctx().emplace<TextEngine>(fontManager, registry);

  const EntityHandle handle = textElement->entityHandle();
  textEngine.prepareForElement(handle, warnings);

  const ResolvedTextFont resolved = textEngine.resolveUsedFont(
      handle, Box2d(Vector2d::Zero(), Vector2d(200, 200)), FontMetrics());
  return DescribeFace(fontManager, resolved.font);
}

std::string TextSvgWithFamily(std::string_view family, std::string_view extraAttributes = "") {
  return std::string(R"(<svg viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">)"
                     R"(<text x="100" y="100" font-size="40" font-family=")")
      .append(family)
      .append("\" ")
      .append(extraAttributes)
      .append(">Text</text></svg>");
}

TEST(FontSelection, GenericFamiliesResolveToVendoredFaces) {
  HermeticFontSet fonts;

  EXPECT_THAT(fonts.select("serif"), FaceFrom("NotoSerif-Regular.ttf"));
  EXPECT_THAT(fonts.select("sans-serif"), FaceFrom("NotoSans-Regular.ttf"));
  EXPECT_THAT(fonts.select("monospace"), FaceFrom("NotoMono-Regular.ttf"));
  EXPECT_THAT(fonts.select("cursive"), FaceFrom("Yellowtail-Regular.ttf"));
  EXPECT_THAT(fonts.select("fantasy"), FaceFrom("SedgwickAveDisplay-Regular.ttf"));
}

TEST(FontSelection, NamedFamiliesResolveToVendoredFaces) {
  HermeticFontSet fonts;

  EXPECT_THAT(fonts.select("Noto Sans"), FaceFrom("NotoSans-Regular.ttf"));
  EXPECT_THAT(fonts.select("Source Sans Pro"), FaceFrom("SourceSansPro-Regular.ttf"));
  EXPECT_THAT(fonts.select("Mplus 1p"), FaceFrom("MPLUS1p-Regular.ttf"));
}

TEST(FontSelection, BoldRequestSelectsTheBoldFace) {
  HermeticFontSet fonts;

  EXPECT_THAT(fonts.select("sans-serif", 700, 0, 5), FaceFrom("NotoSans-Bold.ttf"));
  EXPECT_THAT(fonts.select("Noto Sans", 700, 0, 5), FaceFrom("NotoSans-Bold.ttf"));
}

TEST(FontSelection, ItalicRequestSelectsTheItalicFace) {
  HermeticFontSet fonts;

  EXPECT_THAT(fonts.select("Noto Sans", 400, 1, 5), FaceFrom("NotoSans-Italic.ttf"));
}

TEST(FontSelection, UnknownFamilyFallsBackToTheEmbeddedFace) {
  HermeticFontSet fonts;

  EXPECT_THAT(fonts.select("Invalid"), FaceFrom(kEmbeddedFallback));
}

TEST(FontSelection, DocumentGenericFamilyResolvesThroughTheTextPipeline) {
  EXPECT_THAT(ResolveDocumentFace(TextSvgWithFamily("sans-serif")),
              FaceFrom("NotoSans-Regular.ttf"));
  EXPECT_THAT(ResolveDocumentFace(TextSvgWithFamily("serif")), FaceFrom("NotoSerif-Regular.ttf"));
  EXPECT_THAT(ResolveDocumentFace(TextSvgWithFamily("monospace")),
              FaceFrom("NotoMono-Regular.ttf"));
  EXPECT_THAT(ResolveDocumentFace(TextSvgWithFamily("cursive")),
              FaceFrom("Yellowtail-Regular.ttf"));
  EXPECT_THAT(ResolveDocumentFace(TextSvgWithFamily("fantasy")),
              FaceFrom("SedgwickAveDisplay-Regular.ttf"));
}

TEST(FontSelection, DocumentBoldGenericFamilyResolvesToTheBoldFace) {
  EXPECT_THAT(ResolveDocumentFace(TextSvgWithFamily("sans-serif", R"(font-weight="bold")")),
              FaceFrom("NotoSans-Bold.ttf"));
}

TEST(FontSelection, FamilyListSkipsAnUnknownFamily) {
  EXPECT_THAT(ResolveDocumentFace(TextSvgWithFamily("Invalid, Noto Sans")),
              FaceFrom("NotoSans-Regular.ttf"));
}

TEST(FontSelection, FamilyListPrefersTheFirstAvailableFamily) {
  EXPECT_THAT(ResolveDocumentFace(TextSvgWithFamily("'Source Sans Pro', Noto Sans, serif")),
              FaceFrom("SourceSansPro-Regular.ttf"));
}

TEST(FontSelection, FullyUnknownFamilyListFallsBackToTheEmbeddedFace) {
  EXPECT_THAT(ResolveDocumentFace(TextSvgWithFamily("Invalid")), FaceFrom(kEmbeddedFallback));
}

TEST(FontSelection, QuotedFamilyNameContainingADigitResolves) {
  EXPECT_THAT(ResolveDocumentFace(TextSvgWithFamily("&apos;Mplus 1p&apos;")),
              FaceFrom("MPLUS1p-Regular.ttf"));
}

TEST(FontSelection, UnquotedFamilyNameContainingADigitIsRejected) {
  // `1p` is a dimension token, not an identifier, so the declaration is invalid and font-family
  // keeps its initial `serif` value. Quoting the name is required to select this family.
  EXPECT_THAT(ResolveDocumentFace(TextSvgWithFamily("Mplus 1p")),
              FaceFrom("NotoSerif-Regular.ttf"));
}

TEST(FontSelection, CoverageFallbackSelectsAFaceSupportingTheCodepoint) {
  HermeticFontSet fonts;
  TextEngine engine(fonts.fontManager(), fonts.registry());

  components::ComputedTextComponent text;
  components::ComputedTextComponent::TextSpan span;
  span.text = RcString("\u534a");  // Absent from Noto Sans; only the Japanese face covers it.
  span.start = 0;
  span.end = span.text.size();
  span.startsNewChunk = true;
  text.spans.push_back(span);

  TextLayoutParams params;
  params.fontFamilies.emplace_back(RcString("Noto Sans"));
  params.fontSize = Lengthd(48, Lengthd::Unit::Px);
  params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
  params.fontMetrics = FontMetrics();

  const std::vector<TextRun> runs = engine.layout(text, params);
  ASSERT_THAT(runs, SizeIs(1));
  EXPECT_THAT(DescribeFace(fonts.fontManager(), runs[0].font), FaceFrom("MPLUS1p-Regular.ttf"));
}

}  // namespace
}  // namespace donner::svg
