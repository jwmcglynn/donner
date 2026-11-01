/// @file
/// Joined sideways runs and font-derived decoration bands in vertical text.
#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/css/FontFace.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGGElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/renderer/PlacedTextGeometry.h"
#include "donner/svg/renderer/tests/DecorationTestScene.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/tests/ParserTestUtils.h"
#include "donner/svg/text/TextBackendFull.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg {
namespace {

constexpr std::string_view kArabic = "اليابان";
using RunRecord = std::tuple<int, uint32_t, double, double, double, double>;

MATCHER(VerticalRunRecordNear, "same glyph, source byte, advance, offsets and clockwise rotation") {
  const auto& actual = std::get<0>(arg);
  const auto& expected = std::get<1>(arg);
  return testing::ExplainMatchResult(
      testing::FieldsAre(testing::Eq(std::get<0>(expected)), testing::Eq(std::get<1>(expected)),
                         testing::DoubleNear(std::get<2>(expected), 1e-6),
                         testing::DoubleNear(std::get<3>(expected), 1e-6),
                         testing::DoubleNear(std::get<4>(expected), 1e-6),
                         testing::DoubleNear(std::get<5>(expected), 1e-6)),
      actual, result_listener);
}

const components::ComputedTextGeometryComponent* TextGeometry(SVGDocument& document) {
  const auto text = document.querySelector("#text");
  EXPECT_THAT(text, testing::Optional(testing::_));
  if (!text) {
    return nullptr;
  }
  (void)text->cast<SVGTextElement>().getComputedTextLength();
  auto& engine = document.registry().ctx().get<TextEngine>();
  return &engine.ensureComputedTextGeometryComponent(text->unsafeEntityHandle());
}

std::vector<TextGlyph> AllPositionedGlyphs(SVGDocument& document) {
  std::vector<TextGlyph> glyphs;
  if (const auto* geometry = TextGeometry(document)) {
    for (const auto& run : geometry->runs) {
      glyphs.insert(glyphs.end(), run.glyphs.begin(), run.glyphs.end());
    }
  }
  return glyphs;
}

class MultiAdvanceMarkBackend : public tests::DecorationBackend {
public:
  ShapedRun shapeRun(FontHandle, float, std::string_view, size_t begin, size_t, bool, FontVariant,
                     bool) const override {
    ShapedRun result;
    const uint32_t cluster = static_cast<uint32_t>(begin);
    result.glyphs = {{.glyphIndex = 3, .xAdvance = 10, .cluster = cluster},
                     {.glyphIndex = 4, .xAdvance = 10, .cluster = cluster},
                     {.glyphIndex = 5, .xOffset = -5, .yOffset = -2, .cluster = cluster}};
    return result;
  }
};

class MixedVerticalTextConvergence : public testing::Test {
protected:
  MixedVerticalTextConvergence() : fonts_(registry_), backend_(fonts_, registry_) {}

  void SetUp() override {
    mplus_ = readFont("MPLUS1p-Regular.ttf");
    amiri_ = readFont("Amiri-Regular.ttf");
    addFonts(fonts_);
    amiriFont_ = fonts_.findFont("Mixed Amiri");
    ASSERT_THAT(amiriFont_, testing::Not(testing::Eq(FontHandle())));
    ASSERT_THAT(fonts_.fontData(amiriFont_), testing::ElementsAreArray(*amiri_));
    const FontHandle mplusFont = fonts_.findFont("Mixed Mplus");
    ASSERT_THAT(mplusFont, testing::Not(testing::Eq(FontHandle())));
    ASSERT_THAT(fonts_.fontData(mplusFont), testing::ElementsAreArray(*mplus_));
    TextBackend::ShapingContext::WorkBudget work{4096, 64};
    TextBackend::ShapingContext context;
    context.byteLength = std::string_view("本").size();
    context.work = &work;
    const auto cjk =
        backend_.shapeRunInContext(mplusFont, 20, "本", context, 0, context.byteLength, true,
                                   TextOrientation::Mixed, FontVariant::Normal, false);
    ASSERT_THAT(work.rejected, testing::IsFalse());
    ASSERT_THAT(cjk.glyphs, testing::SizeIs(1));
    ASSERT_THAT(cjk.glyphs[0].glyphIndex, testing::Eq(4977));
    ASSERT_THAT(cjk.glyphs[0].yAdvance, testing::DoubleEq(20));
    cjkAdvance_ = cjk.glyphs[0].yAdvance;
    const auto missing = backend_.shapeRunNoKerning(mplusFont, 20, kArabic, 0, kArabic.size(),
                                                    false, FontVariant::Normal, false);
    ASSERT_THAT(missing.glyphs, testing::Not(testing::IsEmpty()));
    ASSERT_THAT(missing.glyphs,
                testing::Each(testing::Field("glyphIndex", &TextBackend::ShapedGlyph::glyphIndex,
                                             testing::Eq(0))));
    joined_ = shape(kArabic);
    ASSERT_THAT(joined_.glyphs, testing::Not(testing::IsEmpty()));
    std::vector<std::pair<uint32_t, int>> isolated;
    for (size_t byte = 0; byte < kArabic.size(); byte += 2) {
      const auto letter = shape(kArabic.substr(byte, 2));
      ASSERT_THAT(letter.glyphs, testing::Not(testing::IsEmpty()));
      ASSERT_THAT(letter.glyphs,
                  testing::Each(testing::Field("glyphIndex", &TextBackend::ShapedGlyph::glyphIndex,
                                               testing::Gt(0))));
      for (const auto& glyph : letter.glyphs) {
        isolated.emplace_back(static_cast<uint32_t>(byte) + glyph.cluster, glyph.glyphIndex);
      }
    }
    std::vector<std::pair<uint32_t, int>> joinedIds;
    for (const auto& glyph : joined_.glyphs) {
      EXPECT_THAT(glyph.glyphIndex, testing::Gt(0));
      joinedIds.emplace_back(glyph.cluster, glyph.glyphIndex);
    }
    std::sort(joinedIds.begin(), joinedIds.end());
    std::sort(isolated.begin(), isolated.end());
    ASSERT_THAT(joinedIds, testing::Not(testing::ElementsAreArray(isolated)));
  }

  std::shared_ptr<const std::vector<uint8_t>> readFont(std::string_view filename) {
    std::ifstream file(Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts/" +
                                                      std::string(filename)),
                       std::ios::binary);
    EXPECT_THAT(file.is_open(), testing::IsTrue());
    return std::make_shared<const std::vector<uint8_t>>(std::istreambuf_iterator<char>(file),
                                                        std::istreambuf_iterator<char>());
  }

  void addFonts(FontManager& fonts) const {
    for (const auto& [family, bytes] :
         {std::pair{"Mixed Mplus", mplus_}, std::pair{"Mixed Amiri", amiri_}}) {
      css::FontFaceSource source;
      source.kind = css::FontFaceSource::Kind::Data;
      source.payload = bytes;
      source.trusted = true;
      css::FontFace face;
      face.familyName = RcString(family);
      face.sources.push_back(std::move(source));
      fonts.addFontFace(face);
    }
  }

  TextBackend::ShapedRun shape(std::string_view value) const {
    return backend_.shapeRunNoKerning(amiriFont_, 20, value, 0, value.size(), false,
                                      FontVariant::Normal, false);
  }

  SVGDocument document(std::string_view contents, bool mplusFirst = false,
                       std::string_view anchor = "start") const {
    const std::string families =
        mplusFirst ? "Mixed Mplus, Mixed Amiri" : "Mixed Amiri, Mixed Mplus";
    auto result = instantiateSubtree(
        "<text id='text' x='160' y='30' font-family='" + families +
            "' font-size='20' font-kerning='none' writing-mode='tb' text-anchor='" +
            std::string(anchor) + "'>" + std::string(contents) + "</text>",
        {}, {320, 320});
    auto& registry = result.registry();
    auto& fonts = registry.ctx().emplace<FontManager>(registry);
    addFonts(fonts);
    registry.ctx().emplace<TextEngine>(fonts, registry,
                                       std::make_unique<TextBackendFull>(fonts, registry));
    return result;
  }

  std::vector<RunRecord> arabicRecords(
      SVGDocument& document, const std::vector<std::pair<std::string, int>>& sourceOffsets) const {
    const auto* geometry = TextGeometry(document);
    if (!geometry) {
      return {};
    }
    const auto text = document.querySelector("#text");
    const auto& spans =
        document.registry()
            .get<components::ComputedTextComponent>(text->unsafeEntityHandle().entity())
            .spans;
    std::vector<std::pair<TextGlyph, uint32_t>> selected;
    for (size_t runIndex = 0; runIndex < geometry->runs.size(); ++runIndex) {
      const auto& run = geometry->runs[runIndex];
      for (const auto& [selector, offset] : sourceOffsets) {
        const auto source = document.querySelector(selector);
        EXPECT_THAT(source, testing::Optional(testing::_));
        if (!source || spans[runIndex].sourceEntity != source->unsafeEntityHandle().entity()) {
          continue;
        }
        for (const auto& glyph : run.glyphs) {
          const int sourceByte = static_cast<int>(glyph.cluster) + offset;
          if (sourceByte >= 0 && sourceByte < static_cast<int>(kArabic.size())) {
            const auto& fonts = document.registry().ctx().get<FontManager>();
            EXPECT_THAT(fonts.fontData(GlyphFont(run, glyph)), testing::ElementsAreArray(*amiri_));
            selected.emplace_back(glyph, static_cast<uint32_t>(sourceByte));
          }
        }
      }
    }
    std::stable_sort(selected.begin(), selected.end(), [](const auto& a, const auto& b) {
      return a.first.yPosition < b.first.yPosition;
    });
    std::vector<RunRecord> result;
    if (!selected.empty()) {
      const auto& first = selected.front().first;
      for (const auto& [glyph, byte] : selected) {
        result.emplace_back(glyph.glyphIndex, byte, glyph.yAdvance,
                            glyph.xPosition - first.xPosition, glyph.yPosition - first.yPosition,
                            glyph.rotateDegrees);
      }
    }
    return result;
  }

  std::vector<RunRecord> expectedArabicRecords() const {
    std::vector<RunRecord> result;
    const auto& first = joined_.glyphs.front();
    double pen = 0;
    for (const auto& glyph : joined_.glyphs) {
      result.emplace_back(glyph.glyphIndex, glyph.cluster, glyph.xAdvance,
                          first.yOffset - glyph.yOffset, pen + glyph.xOffset - first.xOffset, 90);
      pen += glyph.xAdvance;
    }
    return result;
  }

  double independentPenExtent() const {
    double result = cjkAdvance_;
    for (const auto& value : {shape(" Hi! "), joined_, shape(".")}) {
      for (const auto& glyph : value.glyphs) {
        result += glyph.xAdvance;
      }
    }
    return result;
  }

  void expectCjkAndPrimaryFace(SVGDocument& document, std::string_view primaryFamily) const {
    const auto* geometry = TextGeometry(document);
    ASSERT_THAT(geometry, testing::NotNull());
    auto& fonts = document.registry().ctx().get<FontManager>();
    const FontHandle primary = fonts.findFont(primaryFamily);
    ASSERT_THAT(primary, testing::Not(testing::Eq(FontHandle())));
    std::vector<int> cjk;
    for (const auto& run : geometry->runs) {
      if (!run.glyphs.empty()) {
        EXPECT_THAT(run.font, testing::Eq(primary));
      }
      for (const auto& glyph : run.glyphs) {
        if (glyph.cluster == 0) {
          cjk.push_back(glyph.glyphIndex);
          EXPECT_THAT(glyph.yAdvance, testing::DoubleEq(cjkAdvance_));
          EXPECT_THAT(fonts.fontData(GlyphFont(run, glyph)), testing::ElementsAreArray(*mplus_));
        }
      }
    }
    EXPECT_THAT(cjk, testing::ElementsAre(4977));
  }

  Registry registry_;
  FontManager fonts_;
  TextBackendFull backend_;
  FontHandle amiriFont_;
  std::shared_ptr<const std::vector<uint8_t>> mplus_;
  std::shared_ptr<const std::vector<uint8_t>> amiri_;
  TextBackend::ShapedRun joined_;
  double cjkAdvance_ = 0;
};

TEST_F(MixedVerticalTextConvergence, AmiriPrimaryKeepsJoinedArabicInRotatedSourceOrder) {
  auto actual = document("本 Hi! اليابان.");
  const auto records = arabicRecords(actual, {{"#text", -8}});
  EXPECT_THAT(records, testing::Pointwise(VerticalRunRecordNear(), expectedArabicRecords()));
  const auto text = actual.querySelector("#text")->cast<SVGTextElement>();
  const auto metrics = backend_.fontVMetrics(amiriFont_);
  const double central =
      (metrics.ascent + metrics.descent) * 0.5 * backend_.scaleForEmToPixels(amiriFont_, 20);
  double pen = 30 + cjkAdvance_;
  for (const auto& glyph : shape(" Hi! ").glyphs) {
    pen += glyph.xAdvance;
  }
  for (const auto& glyph : joined_.glyphs) {
    const size_t character = 6 + glyph.cluster / 2;
    const Vector2d origin(160 - central - glyph.yOffset, pen + glyph.xOffset);
    EXPECT_THAT(text.getStartPositionOfChar(character),
                Vector2Eq(testing::DoubleNear(origin.x, 1e-6),
                          testing::DoubleNear(origin.y + glyph.xAdvance, 1e-6)));
    EXPECT_THAT(
        text.getEndPositionOfChar(character),
        Vector2Eq(testing::DoubleNear(origin.x, 1e-6), testing::DoubleNear(origin.y, 1e-6)));
    pen += glyph.xAdvance;
  }
}

TEST_F(MixedVerticalTextConvergence, FallbackArabicUsesTheSameJoinedRotatedRun) {
  auto actual = document("本 Hi! اليابان.", true);
  expectCjkAndPrimaryFace(actual, "Mixed Mplus");
  const auto records = arabicRecords(actual, {{"#text", -8}});
  EXPECT_THAT(records, testing::Pointwise(VerticalRunRecordNear(), expectedArabicRecords()));
}

TEST_F(MixedVerticalTextConvergence, JoiningAndTerminalNeutralSurviveSeparatePaintSpans) {
  auto actual = document(
      "本 <tspan>Hi! </tspan><tspan id='first' fill='red'>الي</tspan>"
      "<tspan id='last' fill='blue'>ابان</tspan><tspan id='period'>.</tspan>");
  const auto records = arabicRecords(actual, {{"#first", 0}, {"#last", 6}});
  EXPECT_THAT(records, testing::Pointwise(VerticalRunRecordNear(), expectedArabicRecords()));
  const auto* geometry = TextGeometry(actual);
  ASSERT_THAT(geometry, testing::NotNull());
  const auto period = actual.querySelector("#period");
  ASSERT_THAT(period, testing::Optional(testing::_));
  const auto& spans = actual.registry()
                          .get<components::ComputedTextComponent>(
                              actual.querySelector("#text")->unsafeEntityHandle().entity())
                          .spans;
  double arabicEnd = 0;
  double periodStart = 0;
  for (size_t i = 0; i < geometry->runs.size(); ++i) {
    for (const auto& glyph : geometry->runs[i].glyphs) {
      if (spans[i].sourceEntity == period->unsafeEntityHandle().entity()) {
        periodStart = glyph.yPosition;
      } else {
        arabicEnd = std::max(arabicEnd, glyph.yPosition + glyph.yAdvance);
      }
    }
  }
  EXPECT_THAT(periodStart, testing::DoubleNear(arabicEnd, 1e-6));
}

TEST_F(MixedVerticalTextConvergence, SidewaysRunMatchesRotatedHorizontalComposition) {
  const FontVMetrics metrics = backend_.fontVMetrics(amiriFont_);
  ASSERT_THAT(metrics.unitsPerEm, testing::Eq(2048));
  ASSERT_THAT(metrics.ascent, testing::Eq(2300));
  ASSERT_THAT(metrics.descent, testing::Eq(-1300));
  const double central =
      (metrics.ascent + metrics.descent) * 0.5 * backend_.scaleForEmToPixels(amiriFont_, 20);
  ASSERT_THAT(central, testing::DoubleEq(4.8828125));
  auto actualDocument = document("Hi! اليابان.");
  auto expectedDocument = instantiateSubtree(
      "<g id='rotated'><text font-family='Mixed Amiri' font-size='20' font-kerning='none'>"
      "Hi! اليابان.</text></g>",
      {}, {320, 320});
  auto& registry = expectedDocument.registry();
  auto& fonts = registry.ctx().emplace<FontManager>(registry);
  addFonts(fonts);
  registry.ctx().emplace<TextEngine>(fonts, registry,
                                     std::make_unique<TextBackendFull>(fonts, registry));
  const auto frame = expectedDocument.querySelector("#rotated");
  ASSERT_THAT(frame, testing::Optional(testing::_));
  const Transform2d documentFromHorizontal =
      Transform2d::Rotate(std::acos(-1.0) * 0.5) * Transform2d::Translate(160 - central, 30);
  frame->cast<SVGGElement>().setTransform(documentFromHorizontal);
  const auto actual = RenderDocumentWithActiveBackend(actualDocument);
  const auto expected = RenderDocumentWithActiveBackend(expectedDocument);
  auto emptyDocument = instantiateSubtree("<g/>", {}, {320, 320});
  const auto transparent = RenderDocumentWithActiveBackend(emptyDocument);
  EXPECT_NONFATAL_FAILURE(
      editor::tests::CompareBitmapToBitmap(expected, transparent, "mixed_rotated_run_visible",
                                           editor::tests::PixelmatchIdentityParams()),
      "pixels differ");
  editor::tests::CompareBitmapToBitmap(actual, expected, "mixed_rotated_horizontal_run",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(MixedVerticalTextConvergence, MiddleAnchorUsesTheIndependentWholeChunkPenExtent) {
  auto start = document("本 Hi! اليابان.");
  auto middle = document("本 Hi! اليابان.", false, "middle");
  expectCjkAndPrimaryFace(start, "Mixed Amiri");
  const auto* startGeometry = TextGeometry(start);
  const auto* middleGeometry = TextGeometry(middle);
  ASSERT_THAT(startGeometry, testing::NotNull());
  ASSERT_THAT(middleGeometry, testing::NotNull());
  ASSERT_THAT(middleGeometry->runs, testing::SizeIs(startGeometry->runs.size()));
  const double extent = independentPenExtent();
  EXPECT_THAT(middle.querySelector("#text")->cast<SVGTextElement>().getComputedTextLength(),
              testing::DoubleNear(extent, 1e-6));
  for (size_t i = 0; i < startGeometry->runs.size(); ++i) {
    const auto& before = startGeometry->runs[i].glyphs;
    const auto& after = middleGeometry->runs[i].glyphs;
    ASSERT_THAT(after, testing::SizeIs(before.size()));
    for (size_t j = 0; j < before.size(); ++j) {
      EXPECT_THAT(Vector2d(after[j].xPosition, after[j].yPosition),
                  Vector2Eq(testing::DoubleNear(before[j].xPosition, 1e-6),
                            testing::DoubleNear(before[j].yPosition - extent / 2, 1e-6)));
    }
  }
}

TEST_F(MixedVerticalTextConvergence, SidewaysMarkOffsetsPreserveAuthoredPenCoordinates) {
  const auto expected = shape("بِ");
  ASSERT_THAT(expected.glyphs, testing::Not(testing::IsEmpty()));
  ASSERT_THAT(
      expected.glyphs,
      testing::Contains(testing::AllOf(
          testing::Field("xAdvance", &TextBackend::ShapedGlyph::xAdvance, testing::DoubleEq(0)),
          testing::Field("xOffset", &TextBackend::ShapedGlyph::xOffset,
                         testing::Not(testing::DoubleEq(0))))));
  const auto metrics = backend_.fontVMetrics(amiriFont_);
  const double central =
      (metrics.ascent + metrics.descent) * 0.5 * backend_.scaleForEmToPixels(amiriFont_, 20);
  for (const bool positioned : {false, true}) {
    SCOPED_TRACE(positioned);
    auto actual = document(positioned ? "<tspan x='190' y='70' dx='3' dy='5'>بِ</tspan>" : "بِ");
    const auto* geometry = TextGeometry(actual);
    ASSERT_THAT(geometry, testing::NotNull());
    std::vector<TextGlyph> glyphs;
    for (const auto& run : geometry->runs) {
      glyphs.insert(glyphs.end(), run.glyphs.begin(), run.glyphs.end());
    }
    ASSERT_THAT(glyphs, testing::SizeIs(expected.glyphs.size()));
    const double penX = positioned ? 193 : 160;
    double penY = positioned ? 75 : 30;
    for (size_t i = 0; i < glyphs.size(); ++i) {
      const auto& oracle = expected.glyphs[i];
      const auto& glyph = glyphs[i];
      EXPECT_THAT(glyph.glyphIndex, testing::Eq(oracle.glyphIndex));
      EXPECT_THAT(Vector2d(glyph.xPosition, glyph.yPosition),
                  Vector2Eq(testing::DoubleNear(penX - central - oracle.yOffset, 1e-6),
                            testing::DoubleNear(penY + oracle.xOffset, 1e-6)));
      EXPECT_THAT(glyph.verticalOriginOffset, testing::DoubleEq(0));
      EXPECT_THAT(glyph.verticalInlineOffset,
                  testing::Optional(testing::DoubleNear(oracle.xOffset, 1e-6)));
      EXPECT_THAT(glyph.verticalCrossOffset, testing::DoubleNear(-central - oracle.yOffset, 1e-6));
      penY += oracle.xAdvance;
    }
  }
}

TEST_F(MixedVerticalTextConvergence, RotatedSidewaysMarkBeforeBaseFollowsClusterFrame) {
  const auto oracle = shape("بِ");
  ASSERT_THAT(
      oracle.glyphs,
      testing::ElementsAre(
          testing::AllOf(
              testing::Field("xAdvance", &TextBackend::ShapedGlyph::xAdvance, testing::DoubleEq(0)),
              testing::Field("cluster", &TextBackend::ShapedGlyph::cluster, 0)),
          testing::AllOf(
              testing::Field("xAdvance", &TextBackend::ShapedGlyph::xAdvance, testing::Gt(0)),
              testing::Field("cluster", &TextBackend::ShapedGlyph::cluster, 0))));
  auto actual = document("<tspan rotate='30'>بِ</tspan>");
  const auto* geometry = TextGeometry(actual);
  ASSERT_THAT(geometry, testing::NotNull());
  std::vector<TextGlyph> glyphs;
  for (const auto& run : geometry->runs) {
    glyphs.insert(glyphs.end(), run.glyphs.begin(), run.glyphs.end());
  }
  ASSERT_THAT(glyphs, testing::SizeIs(2));
  const auto metrics = backend_.fontVMetrics(amiriFont_);
  const double central =
      (metrics.ascent + metrics.descent) * 0.5 * backend_.scaleForEmToPixels(amiriFont_, 20);
  const auto& mark = oracle.glyphs[0];
  const auto& base = oracle.glyphs[1];
  ASSERT_THAT(mark.glyphIndex, testing::Gt(0));
  ASSERT_THAT(base.glyphIndex, testing::Gt(0));
  ASSERT_THAT(mark.glyphIndex, testing::Ne(base.glyphIndex));
  const Vector2d baseOrigin(160 - central - base.yOffset, 30 + base.xOffset);
  const Vector2d markOffset(base.yOffset - mark.yOffset, mark.xOffset - base.xOffset);
  ASSERT_THAT(markOffset.length(), testing::Gt(0));
  const Transform2d authoredFromSideways = Transform2d::Rotate(std::acos(-1.0) / 6);
  const Vector2d markOrigin = baseOrigin + authoredFromSideways.transformPosition(markOffset);
  EXPECT_THAT(glyphs[0].glyphIndex, testing::Eq(mark.glyphIndex));
  EXPECT_THAT(glyphs[1].glyphIndex, testing::Eq(base.glyphIndex));
  EXPECT_THAT(
      Vector2d(glyphs[0].xPosition, glyphs[0].yPosition),
      Vector2Eq(testing::DoubleNear(markOrigin.x, 1e-6), testing::DoubleNear(markOrigin.y, 1e-6)));
  EXPECT_THAT(
      Vector2d(glyphs[1].xPosition, glyphs[1].yPosition),
      Vector2Eq(testing::DoubleNear(baseOrigin.x, 1e-6), testing::DoubleNear(baseOrigin.y, 1e-6)));
  for (const auto& glyph : glyphs) {
    EXPECT_THAT(glyph.rotateDegrees, testing::DoubleEq(120));
  }
}

TEST_F(MixedVerticalTextConvergence, RotatedPaintSpanMarksMatchHorizontalAttachmentFrame) {
  const auto oracle = shape("بِ");
  ASSERT_THAT(oracle.glyphs, testing::SizeIs(2));
  const auto& mark = oracle.glyphs[0];
  const auto& base = oracle.glyphs[1];
  ASSERT_THAT(mark.xAdvance, testing::DoubleEq(0));
  ASSERT_THAT(base.xAdvance, testing::Gt(0));
  ASSERT_THAT(mark.glyphIndex, testing::Gt(0));
  ASSERT_THAT(base.glyphIndex, testing::Gt(0));
  ASSERT_THAT(mark.glyphIndex, testing::Ne(base.glyphIndex));
  const Vector2d markOffset(base.yOffset - mark.yOffset, mark.xOffset - base.xOffset);
  ASSERT_THAT(markOffset.length(), testing::Gt(0));
  auto horizontal = document("<tspan fill='red'>ب</tspan><tspan fill='blue'>ِ</tspan>");
  auto horizontalText = horizontal.querySelector("#text");
  ASSERT_THAT(horizontalText, testing::Optional(testing::_));
  horizontalText->setAttribute("writing-mode", "horizontal-tb");
  horizontalText->setAttribute("x", "0");
  horizontalText->setAttribute("y", "0");
  const auto* horizontalGeometry = TextGeometry(horizontal);
  ASSERT_THAT(horizontalGeometry, testing::NotNull());
  std::vector<TextGlyph> horizontalGlyphs;
  for (const auto& run : horizontalGeometry->runs) {
    horizontalGlyphs.insert(horizontalGlyphs.end(), run.glyphs.begin(), run.glyphs.end());
  }
  ASSERT_THAT(horizontalGlyphs, testing::SizeIs(2));
  ASSERT_THAT(horizontalGlyphs,
              testing::Contains(testing::Field("glyphIndex", &TextGlyph::glyphIndex,
                                               testing::Eq(mark.glyphIndex))));
  ASSERT_THAT(horizontalGlyphs,
              testing::Contains(testing::Field("glyphIndex", &TextGlyph::glyphIndex,
                                               testing::Eq(base.glyphIndex))));
  const auto horizontalMark =
      std::find_if(horizontalGlyphs.begin(), horizontalGlyphs.end(),
                   [&](const TextGlyph& glyph) { return glyph.glyphIndex == mark.glyphIndex; });
  const auto horizontalBase =
      std::find_if(horizontalGlyphs.begin(), horizontalGlyphs.end(),
                   [&](const TextGlyph& glyph) { return glyph.glyphIndex == base.glyphIndex; });
  auto actual = document(
      "<tspan rotate='30'><tspan fill='red'>ب</tspan>"
      "<tspan fill='blue'>ِ</tspan></tspan>");
  const auto* geometry = TextGeometry(actual);
  ASSERT_THAT(geometry, testing::NotNull());
  std::vector<TextGlyph> glyphs;
  for (const auto& run : geometry->runs) {
    glyphs.insert(glyphs.end(), run.glyphs.begin(), run.glyphs.end());
  }
  ASSERT_THAT(glyphs, testing::SizeIs(2));
  ASSERT_THAT(glyphs, testing::Contains(testing::Field("glyphIndex", &TextGlyph::glyphIndex,
                                                       testing::Eq(mark.glyphIndex))));
  ASSERT_THAT(glyphs, testing::Contains(testing::Field("glyphIndex", &TextGlyph::glyphIndex,
                                                       testing::Eq(base.glyphIndex))));
  const auto actualMark = std::find_if(glyphs.begin(), glyphs.end(), [&](const TextGlyph& glyph) {
    return glyph.glyphIndex == mark.glyphIndex;
  });
  const auto actualBase = std::find_if(glyphs.begin(), glyphs.end(), [&](const TextGlyph& glyph) {
    return glyph.glyphIndex == base.glyphIndex;
  });
  const auto metrics = backend_.fontVMetrics(amiriFont_);
  const double central =
      (metrics.ascent + metrics.descent) * 0.5 * backend_.scaleForEmToPixels(amiriFont_, 20);
  // This control preserves horizontal split-span attachment; the one-span oracle above
  // separately checks the complete font-shaped cluster.
  const Vector2d baseOrigin(160 - central - horizontalBase->yPosition,
                            30 + horizontalBase->xPosition);
  const Vector2d horizontalAttachment(horizontalBase->yPosition - horizontalMark->yPosition,
                                      horizontalMark->xPosition - horizontalBase->xPosition);
  RecordProperty("horizontal_split_attachment_length",
                 std::to_string(horizontalAttachment.length()));
  const Transform2d authoredFromSideways = Transform2d::Rotate(std::acos(-1.0) / 6);
  const Vector2d markOrigin =
      baseOrigin + authoredFromSideways.transformPosition(horizontalAttachment);
  EXPECT_THAT(
      Vector2d(actualMark->xPosition, actualMark->yPosition),
      Vector2Eq(testing::DoubleNear(markOrigin.x, 1e-6), testing::DoubleNear(markOrigin.y, 1e-6)));
  EXPECT_THAT(
      Vector2d(actualBase->xPosition, actualBase->yPosition),
      Vector2Eq(testing::DoubleNear(baseOrigin.x, 1e-6), testing::DoubleNear(baseOrigin.y, 1e-6)));
}

TEST_F(MixedVerticalTextConvergence, JoinerDoesNotMoveALaterClustersRotationPivot) {
  const auto oracle = shape("B́");
  ASSERT_THAT(oracle.glyphs, testing::SizeIs(2));
  const auto& base = oracle.glyphs[0];
  const auto& mark = oracle.glyphs[1];
  ASSERT_THAT(base.xAdvance, testing::Gt(0));
  ASSERT_THAT(mark.xAdvance, testing::DoubleEq(0));
  ASSERT_THAT(base.glyphIndex, testing::Gt(0));
  ASSERT_THAT(mark.glyphIndex, testing::Gt(0));
  ASSERT_THAT(mark.glyphIndex, testing::Ne(base.glyphIndex));
  auto before = document("A&#x200d;B́ ب");
  auto after = document("<tspan rotate='30'>A&#x200d;B́</tspan> ب");
  const auto original = AllPositionedGlyphs(before);
  const auto rotated = AllPositionedGlyphs(after);
  const auto hasBase = testing::Contains(
      testing::Field("glyphIndex", &TextGlyph::glyphIndex, testing::Eq(base.glyphIndex)));
  const auto hasMark = testing::Contains(
      testing::Field("glyphIndex", &TextGlyph::glyphIndex, testing::Eq(mark.glyphIndex)));
  ASSERT_THAT(original, testing::AllOf(hasBase, hasMark));
  ASSERT_THAT(rotated, testing::AllOf(hasBase, hasMark));
  const auto find = [](const auto& glyphs, int id) {
    return std::find_if(glyphs.begin(), glyphs.end(),
                        [&](const TextGlyph& glyph) { return glyph.glyphIndex == id; });
  };
  const auto oldBase = find(original, base.glyphIndex);
  const auto oldMark = find(original, mark.glyphIndex);
  const auto newBase = find(rotated, base.glyphIndex);
  const auto newMark = find(rotated, mark.glyphIndex);
  ASSERT_THAT(oldBase->yAdvance, testing::Gt(0));
  ASSERT_THAT(oldMark->yAdvance, testing::DoubleEq(0));
  const Vector2d offset(oldMark->xPosition - oldBase->xPosition,
                        oldMark->yPosition - oldBase->yPosition);
  ASSERT_THAT(offset.length(), testing::Gt(0));
  const Transform2d authoredFromSideways = Transform2d::Rotate(std::acos(-1.0) / 6);
  const Vector2d expected = Vector2d(newBase->xPosition, newBase->yPosition) +
                            authoredFromSideways.transformPosition(offset);
  EXPECT_THAT(Vector2d(newBase->xPosition, newBase->yPosition),
              Vector2Eq(testing::DoubleNear(oldBase->xPosition, 1e-6),
                        testing::DoubleNear(oldBase->yPosition, 1e-6)));
  EXPECT_THAT(
      Vector2d(newMark->xPosition, newMark->yPosition),
      Vector2Eq(testing::DoubleNear(expected.x, 1e-6), testing::DoubleNear(expected.y, 1e-6)));
}

TEST(MixedVerticalDecoration, MultiAdvanceClusterRetainsPreviousGlyphMarkPivot) {
  MultiAdvanceMarkBackend probe;
  const auto scripted = probe.shapeRun({}, 20, "ب", 0, 2, true, FontVariant::Normal, false);
  ASSERT_THAT(
      scripted.glyphs,
      testing::ElementsAre(
          testing::AllOf(testing::Field("glyphIndex", &TextBackend::ShapedGlyph::glyphIndex, 3),
                         testing::Field("xAdvance", &TextBackend::ShapedGlyph::xAdvance, 10)),
          testing::AllOf(testing::Field("glyphIndex", &TextBackend::ShapedGlyph::glyphIndex, 4),
                         testing::Field("xAdvance", &TextBackend::ShapedGlyph::xAdvance, 10)),
          testing::AllOf(testing::Field("glyphIndex", &TextBackend::ShapedGlyph::glyphIndex, 5),
                         testing::Field("xAdvance", &TextBackend::ShapedGlyph::xAdvance, 0),
                         testing::Field("xOffset", &TextBackend::ShapedGlyph::xOffset, -5),
                         testing::Field("yOffset", &TextBackend::ShapedGlyph::yOffset, -2))));
  auto document = instantiateSubtree(
      "<text id='text' x='80' y='10' font-size='20' writing-mode='vertical-rl' rotate='30'>"
      "ب</text>",
      {}, {100, 100});
  auto& registry = document.registry();
  auto& fonts = registry.ctx().emplace<FontManager>(registry);
  auto& engine = registry.ctx().emplace<TextEngine>(fonts, registry,
                                                    std::make_unique<MultiAdvanceMarkBackend>());
  const auto glyphs = AllPositionedGlyphs(document);
  ASSERT_THAT(glyphs, testing::SizeIs(3));
  EXPECT_THAT(glyphs,
              testing::ElementsAre(testing::Field("glyphIndex", &TextGlyph::glyphIndex, 3),
                                   testing::Field("glyphIndex", &TextGlyph::glyphIndex, 4),
                                   testing::Field("glyphIndex", &TextGlyph::glyphIndex, 5)));
  const double central = 300 * engine.scaleForEmToPixels({}, 20);
  const Vector2d pivot(80 - central, 20);
  const Transform2d authoredFromSideways = Transform2d::Rotate(std::acos(-1.0) / 6);
  const Vector2d expected = pivot + authoredFromSideways.transformPosition({2, 5});
  EXPECT_THAT(Vector2d(glyphs[1].xPosition, glyphs[1].yPosition),
              Vector2Eq(testing::DoubleNear(pivot.x, 1e-6), testing::DoubleNear(pivot.y, 1e-6)));
  EXPECT_THAT(
      Vector2d(glyphs[2].xPosition, glyphs[2].yPosition),
      Vector2Eq(testing::DoubleNear(expected.x, 1e-6), testing::DoubleNear(expected.y, 1e-6)));
}

TEST_F(MixedVerticalTextConvergence, VerticalLengthOwnerRetainsItsExistingLayoutProfile) {
  auto supported = document(kArabic);
  ASSERT_THAT(TextGeometry(supported), testing::NotNull());
  EXPECT_THAT(supported.registry().ctx().get<TextEngine>().bidiRunCandidatesForTesting(),
              testing::Gt(0));
  auto owned = document("<tspan textLength='60' lengthAdjust='spacingAndGlyphs'>اليابان</tspan>");
  const auto* geometry = TextGeometry(owned);
  ASSERT_THAT(geometry, testing::NotNull());
  EXPECT_THAT(owned.querySelector("#text")->cast<SVGTextElement>().getComputedTextLength(),
              testing::DoubleNear(60, 1e-6));
  EXPECT_THAT(owned.registry().ctx().get<TextEngine>().bidiRunCandidatesForTesting(),
              testing::Eq(0));
}

TEST(MixedVerticalDecoration, DeclaringFontBandUsesTheVerticalPenFrame) {
  for (std::string_view mode : {"vertical-rl", "vertical-lr"}) {
    SCOPED_TRACE(mode);
    auto document =
        tests::DecorationDocument("<text id='text' x='80' y='10' font-size='125' writing-mode='" +
                                  std::string(mode) + "'>X日X</text>");
    const auto* geometry = TextGeometry(document);
    ASSERT_THAT(geometry, testing::NotNull());
    std::vector<TextRun> runs;
    for (const auto& run : geometry->runs) {
      if (!run.glyphs.empty()) {
        runs.push_back(run);
      }
    }
    ASSERT_THAT(runs, testing::SizeIs(1));
    TextRun run = runs[0];
    ASSERT_THAT(run.glyphs, testing::SizeIs(3));
    TextGlyph mark = run.glyphs[1];
    mark.yAdvance = 0;
    run.glyphs.push_back(mark);
    components::TextDecorationOrigin origin;
    origin.lines = TextDecoration::Underline;
    origin.font = ResolvedTextFont{run.font, 125};
    std::size_t remaining = Path::kMaximumGeometryQueryWork;
    std::vector<Path> paths;
    const auto& engine = document.registry().ctx().get<TextEngine>();
    EmitTextDecorationPaths(run, origin, origin.lines, 1, 125, engine, Transform2d(), remaining,
                            [&](const Path& path) { paths.push_back(path); });
    ASSERT_THAT(paths, testing::SizeIs(1));
    EXPECT_THAT(paths[0].bounds(), testing::Eq(Box2d::FromXYWH(26.875, 10, 6.25, 30)));
    remaining = 0;
    paths.clear();
    EmitTextDecorationPaths(run, origin, origin.lines, 1, 125, engine, Transform2d(), remaining,
                            [&](const Path& path) { paths.push_back(path); });
    EXPECT_THAT(paths, testing::IsEmpty());
  }
}

TEST(MixedVerticalDecoration, SidewaysUnderlineMatchesAnExplicitFontDerivedBand) {
  // At exactly 1/8 em scale the declaring font's central offset is 37.5, underline offset is
  // 12.5, and thickness is 6.25. Clockwise sideways layout maps the alphabetic under side left.
  constexpr std::string_view kGlyphs =
      "<rect x='42.5' y='10' width='2' height='1'/>"
      "<rect x='42.5' y='20' width='2' height='1'/>";
  for (std::string_view mode : {"vertical-rl", "vertical-lr"}) {
    SCOPED_TRACE(mode);
    const auto actual =
        tests::RenderDecorationScene("<text x='80' y='10' font-size='125' writing-mode='" +
                                     std::string(mode) + "' text-decoration='underline'>XX</text>");
    const auto expected = tests::RenderDecorationScene(
        std::string(kGlyphs) + "<rect x='26.875' y='10' width='6.25' height='20'/>");
    const auto wrongSide = tests::RenderDecorationScene(
        std::string(kGlyphs) + "<rect x='53.125' y='10' width='6.25' height='20'/>");
    EXPECT_NONFATAL_FAILURE(
        editor::tests::CompareBitmapToBitmap(expected, wrongSide, "vertical_band_wrong_side",
                                             editor::tests::PixelmatchIdentityParams()),
        "pixels differ");
    editor::tests::CompareBitmapToBitmap(actual, expected, "vertical_font_underline_band",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(MixedVerticalDecoration, AuthoredHorizontalRotationRetainsSeparateGlyphBands) {
  const auto actual = tests::RenderDecorationScene(
      "<text x='20' y='20' font-size='20' rotate='90' text-decoration='underline'>XX</text>");
  const auto expected = tests::RenderDecorationScene(
      "<rect x='20' y='20' width='2' height='1'/><rect x='30' y='20' width='2' height='1'/>"
      "<rect x='17.5' y='20' width='1' height='10'/>"
      "<rect x='27.5' y='20' width='1' height='10'/>");
  const auto transparent = tests::RenderDecorationScene("");
  EXPECT_NONFATAL_FAILURE(
      editor::tests::CompareBitmapToBitmap(expected, transparent, "horizontal_rotated_band_visible",
                                           editor::tests::PixelmatchIdentityParams()),
      "pixels differ");
  editor::tests::CompareBitmapToBitmap(actual, expected, "horizontal_authored_rotation_bands",
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
