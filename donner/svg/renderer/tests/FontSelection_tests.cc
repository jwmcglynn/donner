/// @file
/// Face-selection coverage for the hermetic test font set.
///
/// These tests assert which font file a family request resolves to, independently of any pixel
/// comparison, so a selection regression is reported as a named face mismatch instead of a diff
/// count.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Utf8.h"
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

using ::testing::AllOf;
using ::testing::Each;
using ::testing::ElementsAre;
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

TEST(FontSelection, BoldItalicRequestSelectsAvailableItalicFaceBeforeUprightBold) {
  // The vendored set has separate Bold and Italic files, but no BoldItalic file. CSS face
  // matching prioritizes style over weight; this identifies the face, not synthetic-bold output.
  HermeticFontSet fonts;
  const SelectedFace expected{"NotoSans-Italic.ttf", "Noto Sans", 400, 1};
  EXPECT_EQ(fonts.select("Noto Sans", 700, 1, 5), expected);
  EXPECT_EQ(ResolveDocumentFace(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 80 60">
      <text x="4" y="38" font="bold italic 28 Noto Sans">Ab</text>
    </svg>)svg"),
            expected);
}

TEST(FontSelection, OnlyRegularAndBoldWeightsAreRegistered) {
  // Thin, Light and Black Noto Sans files are vendored but deliberately not registered, so light
  // requests resolve to Regular and heavy ones to Bold.
  HermeticFontSet fonts;

  EXPECT_THAT(fonts.select("Noto Sans", 100, 0, 5), FaceFrom("NotoSans-Regular.ttf"));
  EXPECT_THAT(fonts.select("Noto Sans", 300, 0, 5), FaceFrom("NotoSans-Regular.ttf"));
  EXPECT_THAT(fonts.select("Noto Sans", 900, 0, 5), FaceFrom("NotoSans-Bold.ttf"));
}

TEST(FontSelection, FamilyInheritedFromTheRootResolvesToTheRegularFace) {
  const SelectedFace expected{"NotoSans-Regular.ttf", "Noto Sans", 400, 0};
  EXPECT_EQ(ResolveDocumentFace(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 120 40" font-family="Noto Sans">
      <text x="4" y="32">label</text>
    </svg>)svg"),
            expected);
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
  ASSERT_THAT(runs[0].glyphs, SizeIs(1));
  // The run keeps the span's face; the glyph it cannot render carries the covering face.
  EXPECT_THAT(DescribeFace(fonts.fontManager(), runs[0].font), FaceFrom("NotoSans-Regular.ttf"));
  EXPECT_THAT(DescribeFace(fonts.fontManager(), GlyphFont(runs[0], runs[0].glyphs[0])),
              FaceFrom("MPLUS1p-Regular.ttf"));
}

/// The face that draws one laid-out glyph, identified by the source codepoint it starts at.
struct GlyphFace {
  char32_t codepoint = 0;  ///< First codepoint of the glyph's source cluster.
  std::string source;      ///< Vendored font filename, or \ref kEmbeddedFallback.
  bool drawn = true;       ///< False when the glyph is `.notdef`, which renderers skip.

  bool operator==(const GlyphFace&) const = default;
};

std::ostream& operator<<(std::ostream& os, const GlyphFace& face) {
  return os << "GlyphFace{U+" << std::hex << static_cast<uint32_t>(face.codepoint) << std::dec
            << ", source=\"" << face.source << "\", drawn=" << face.drawn << "}";
}

/**
 * Lays out @p utf8 as one span and reports the face that draws each glyph, in source order, so a
 * per-cluster selection error names the cluster and the face instead of a pixel count.
 *
 * Glyphs are sorted by source cluster because the full-text tier returns right-to-left glyphs in
 * visual order while the simple tier keeps logical order.
 */
std::vector<GlyphFace> LayOutGlyphFaces(std::string_view utf8,
                                        std::initializer_list<const char*> families) {
  HermeticFontSet fonts;
  TextEngine engine(fonts.fontManager(), fonts.registry());

  components::ComputedTextComponent text;
  components::ComputedTextComponent::TextSpan span;
  span.text = RcString(utf8);
  span.start = 0;
  span.end = span.text.size();
  span.startsNewChunk = true;
  text.spans.push_back(span);

  TextLayoutParams params;
  for (const char* family : families) {
    params.fontFamilies.emplace_back(RcString(family));
  }
  params.fontSize = Lengthd(32, Lengthd::Unit::Px);
  params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
  params.fontMetrics = FontMetrics();

  const std::vector<TextRun> runs = engine.layout(text, params);
  EXPECT_THAT(runs, SizeIs(1));
  if (runs.empty()) {
    return {};
  }

  std::vector<TextGlyph> glyphs = runs[0].glyphs;
  std::stable_sort(glyphs.begin(), glyphs.end(),
                   [](const TextGlyph& a, const TextGlyph& b) { return a.cluster < b.cluster; });
  std::vector<GlyphFace> result;
  for (const TextGlyph& glyph : glyphs) {
    const char32_t codepoint = std::get<0>(Utf8::NextCodepoint(utf8.substr(glyph.cluster)));
    result.push_back({codepoint,
                      DescribeFace(fonts.fontManager(), GlyphFont(runs[0], glyph)).source,
                      glyph.glyphIndex != 0});
  }
  return result;
}

TEST(FontSelection, PerClusterFallbackKeepsThePrimaryFaceForClustersItCovers) {
  // Mplus 1p covers the CJK, the space and nothing Arabic; only Amiri covers the Arabic word, so
  // only those clusters fall back. A whole-span choice would either drop the Arabic glyphs or draw
  // the CJK in a face without them.
  EXPECT_THAT(
      LayOutGlyphFaces("\u65e5\u672c \u062f\u0627\u0644", {"Mplus 1p"}),
      ElementsAre(
          GlyphFace{U'\u65e5', "MPLUS1p-Regular.ttf"}, GlyphFace{U'\u672c', "MPLUS1p-Regular.ttf"},
          GlyphFace{U' ', "MPLUS1p-Regular.ttf"}, GlyphFace{U'\u062f', "Amiri-Regular.ttf"},
          GlyphFace{U'\u0627', "Amiri-Regular.ttf"}, GlyphFace{U'\u0644', "Amiri-Regular.ttf"}));
}

TEST(FontSelection, PerClusterFallbackPrefersTheNextListedFamilyThatCoversTheCluster) {
  // Noto Emoji has no Latin letters. Every vendored text face covers `A`, so the listed family
  // must win over the registration order of the other faces.
  EXPECT_THAT(LayOutGlyphFaces("\U0001F600A", {"Noto Emoji", "Yellowtail"}),
              ElementsAre(GlyphFace{U'\U0001F600', "NotoEmoji-Regular.ttf"},
                          GlyphFace{U'A', "Yellowtail-Regular.ttf"}));
}

TEST(FontSelection, PerClusterFallbackMovesACombiningMarkWithItsBase) {
  // Yellowtail lacks U+0301, and `q` has no precomposed acute form. The base and its mark fall
  // back together to the next listed family covering both; the neighbors keep Yellowtail. The
  // full-text tier reports the mark glyph under its base's cluster, so only the ends are fixed.
  const std::vector<GlyphFace> faces = LayOutGlyphFaces("aq\u0301b", {"Yellowtail", "Noto Serif"});
  ASSERT_THAT(faces, SizeIs(4));
  EXPECT_THAT(faces.front(), Eq(GlyphFace{U'a', "Yellowtail-Regular.ttf"}));
  EXPECT_THAT(faces.back(), Eq(GlyphFace{U'b', "Yellowtail-Regular.ttf"}));
  EXPECT_THAT(std::vector<GlyphFace>(faces.begin() + 1, faces.end() - 1),
              Each(AllOf(Field("source", &GlyphFace::source, Eq("NotoSerif-Regular.ttf")),
                         Field("drawn", &GlyphFace::drawn, Eq(true)))));
}

TEST(FontSelection, PerClusterFallbackKeepsAnIndicSyllableInOneFace) {
  // Yellowtail has no Devanagari. Every glyph of the syllable, including the virama, the second
  // consonant and the vowel sign that a shaper merges into the syllable's cluster, must come from
  // the face that covers it and must not be `.notdef`.
  const std::vector<GlyphFace> faces =
      LayOutGlyphFaces("a\u0938\u094d\u0924\u0947", {"Yellowtail", "Noto Sans Devanagari"});
  ASSERT_THAT(faces, SizeIs(testing::Ge(2u)));
  EXPECT_THAT(faces.front(), Eq(GlyphFace{U'a', "Yellowtail-Regular.ttf"}));
  EXPECT_THAT(std::vector<GlyphFace>(faces.begin() + 1, faces.end()),
              Each(AllOf(Field("source", &GlyphFace::source, Eq("NotoSansDevanagari-Regular.ttf")),
                         Field("drawn", &GlyphFace::drawn, Eq(true)))));
}

/// A font manager with a face that covers no Latin letters as "Primary", \p decoys more such faces,
/// and Yellowtail as "Covering". Spans list the families in that order, so a Latin cluster's
/// fallback search tries every decoy before the covering face.
class FallbackBudgetFontSet {
public:
  explicit FallbackBudgetFontSet(int decoys) : fontManager_(registry_) {
    add("NotoEmoji-Regular.ttf", "Primary");
    for (int index = 0; index < decoys; ++index) {
      // Distinct trailing padding keeps identical font bytes from sharing one loaded face.
      add("NotoEmoji-Regular.ttf", "Decoy" + std::to_string(index),
          4u * static_cast<size_t>(index + 1));
    }
    add("Yellowtail-Regular.ttf", "Covering");
  }

  /// Lays out \p utf8 in "Primary" and reports the face that draws each glyph.
  std::vector<GlyphFace> layOut(std::string_view utf8) {
    TextEngine engine(fontManager_, registry_);
    components::ComputedTextComponent text;
    components::ComputedTextComponent::TextSpan span;
    span.text = RcString(utf8);
    span.start = 0;
    span.end = span.text.size();
    span.startsNewChunk = true;
    text.spans.push_back(span);
    TextLayoutParams params;
    params.fontFamilies = families_;
    params.fontSize = Lengthd(32, Lengthd::Unit::Px);
    params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
    params.fontMetrics = FontMetrics();
    const std::vector<TextRun> runs = engine.layout(text, params);
    coverageEntries_ = engine.fallbackCoverageEntriesForTesting();
    EXPECT_THAT(runs, SizeIs(1));
    std::vector<GlyphFace> result;
    for (const TextRun& run : runs) {
      for (const TextGlyph& glyph : run.glyphs) {
        result.push_back({std::get<0>(Utf8::NextCodepoint(utf8.substr(glyph.cluster))),
                          DescribeFace(fontManager_, GlyphFont(run, glyph)).source,
                          glyph.glyphIndex != 0});
      }
    }
    return result;
  }

  /// Coverage answers fallback retained during the last \ref layOut.
  std::size_t coverageEntries() const { return coverageEntries_; }

private:
  void add(const std::string& file, const std::string& family, size_t padding = 0) {
    std::vector<uint8_t> bytes = ReadFile(TestFontsDir() / file);
    bytes.resize(bytes.size() + padding, 0);
    css::FontFaceSource source;
    source.kind = css::FontFaceSource::Kind::Data;
    source.payload = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    source.trusted = true;
    css::FontFace face;
    face.familyName = RcString(family);
    face.sources.push_back(std::move(source));
    fontManager_.addFontFace(face);
    families_.emplace_back(RcString(family));
  }

  Registry registry_;
  FontManager fontManager_;
  SmallVector<RcString, 1> families_;
  std::size_t coverageEntries_ = 0;
};

TEST(FontSelection, FallbackSearchesEveryCandidateFaceForAShortSpan) {
  // One Latin letter must reach the covering face after 40 faces that lack it, even though that
  // takes more coverage queries than a one-byte span's per-byte grant.
  FallbackBudgetFontSet fonts(40);
  EXPECT_THAT(fonts.layOut("B"), ElementsAre(GlyphFace{U'B', "Yellowtail-Regular.ttf", true}));
}

TEST(FontSelection, FallbackStopsSearchingOnceTheQueryGrantIsSpent) {
  // Each distinct Latin letter costs about 42 coverage queries (the primary face, 40 decoys and
  // the covering face) against a grant of 32 per byte, so early letters fall back and the last
  // ones stay `.notdef` on the primary face.
  FallbackBudgetFontSet fonts(40);
  const std::vector<GlyphFace> faces = fonts.layOut("BCDEFGHIJKLMNOPQ");
  ASSERT_THAT(faces, SizeIs(16));
  EXPECT_THAT(faces.front(), Eq(GlyphFace{U'B', "Yellowtail-Regular.ttf", true}));
  EXPECT_THAT(faces.back(), Eq(GlyphFace{U'Q', "NotoEmoji-Regular.ttf", false}));
}

TEST(FontSelection, FallbackCoverageMemoryStaysBoundedForManyDistinctCodepoints) {
  // 1000 distinct CJK codepoints that no registered face covers, against 41 candidate faces: the
  // query grant allows every lookup, but the retained answers must stay within a fixed bound so a
  // document cannot grow fallback memory with its text length times its face count.
  FallbackBudgetFontSet fonts(40);
  std::string text;
  for (char32_t codepoint = U'一'; codepoint < U'一' + 1000; ++codepoint) {
    Utf8::Append(codepoint, std::back_inserter(text));
  }
  const std::vector<GlyphFace> faces = fonts.layOut(text);
  ASSERT_THAT(faces, SizeIs(1000));
  EXPECT_THAT(faces, Each(Field("drawn", &GlyphFace::drawn, Eq(false))));
  EXPECT_THAT(fonts.coverageEntries(), testing::Le(TextEngine::kMaximumFallbackCoverageEntries));
}

TEST(FontSelection, FallbackFaceKernsAcrossAPaintOnlySpanBoundary) {
  // Noto Emoji has no Latin, so both letters of "AV" fall back to Noto Sans, which kerns the pair.
  // Splitting the text into two spans that differ only in paint must not change the spacing.
  HermeticFontSet fonts;
  TextEngine engine(fonts.fontManager(), fonts.registry());
  TextLayoutParams params;
  params.fontFamilies.emplace_back(RcString("Noto Emoji"));
  params.fontFamilies.emplace_back(RcString("Noto Sans"));
  params.fontSize = Lengthd(64, Lengthd::Unit::Px);
  params.viewBox = Box2d(Vector2d::Zero(), Vector2d(200, 200));
  params.fontMetrics = FontMetrics();

  const auto makeSpan = [](std::string_view utf8, bool startsNewChunk) {
    components::ComputedTextComponent::TextSpan span;
    span.text = RcString(utf8);
    span.start = 0;
    span.end = span.text.size();
    span.startsNewChunk = startsNewChunk;
    return span;
  };
  components::ComputedTextComponent unsplit;
  unsplit.spans.push_back(makeSpan("AV", true));
  components::ComputedTextComponent split;
  split.spans.push_back(makeSpan("A", true));
  auto paintOnly = makeSpan("V", false);
  paintOnly.paintOrder.order = {PaintComponent::Stroke, PaintComponent::Fill,
                                PaintComponent::Markers};
  split.spans.push_back(std::move(paintOnly));

  const auto glyphXs = [&](const components::ComputedTextComponent& text) {
    std::vector<double> xs;
    for (const TextRun& run : engine.layout(text, params)) {
      for (const TextGlyph& glyph : run.glyphs) {
        EXPECT_THAT(DescribeFace(fonts.fontManager(), GlyphFont(run, glyph)),
                    FaceFrom("NotoSans-Regular.ttf"));
        xs.push_back(glyph.xPosition);
      }
    }
    return xs;
  };
  const std::vector<double> unsplitXs = glyphXs(unsplit);
  ASSERT_THAT(unsplitXs, SizeIs(2));
  EXPECT_THAT(glyphXs(split), testing::Pointwise(testing::DoubleNear(1e-9), unsplitXs));
}

}  // namespace
}  // namespace donner::svg
