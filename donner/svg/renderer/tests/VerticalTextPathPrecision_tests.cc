/// @file
/// Independent arc-frame controls for upright text placed on cubic paths.
#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/Path.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/css/FontFace.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/tests/ParserTestUtils.h"
#include "donner/svg/text/TextBackendFull.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg {
namespace {

constexpr std::array<int, 6> kGlyphs{4868, 4977, 7128, 2848, 6983, 5800};

struct IndependentFrame {
  Vector2d midpoint;
  double tangentRadians;
};

// Independent 64-point Gauss-Legendre integration and arc inversion at distances
// 12, 36, 60, 84, 108, 132; no production path sampler supplies these frames.
const std::array<IndependentFrame, 6> kCurvedFrames{{
    {{26.125007728981142, 110.27659862792211}, 0.8958525859564147},
    {{45.52170654282067, 123.85698934133045}, 0.3294777460862565},
    {{69.19402549301724, 125.30413914396655}, -0.20552600283065},
    {{90.18122251189686, 114.32254963011306}, -0.7655488814110843},
    {{102.98162678613275, 94.21060140343035}, -1.0228105676428811},
    {{120.45967049052153, 78.23869589768273}, -0.45601105036319695},
}};

uint16_t ReadUint16(std::span<const uint8_t> data, size_t offset) {
  if (offset > data.size() || data.size() - offset < 2) {
    ADD_FAILURE() << "font metric at " << offset << " exceeds table size " << data.size();
    return 0;
  }
  return (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
}

int16_t ReadInt16(std::span<const uint8_t> data, size_t offset) {
  return static_cast<int16_t>(ReadUint16(data, offset));
}

uint32_t ReadUint32(std::span<const uint8_t> data, size_t offset) {
  return (static_cast<uint32_t>(ReadUint16(data, offset)) << 16) | ReadUint16(data, offset + 2);
}

class VerticalTextPathPrecision : public testing::Test {
protected:
  struct Occurrence {
    Path outline;
    Vector2d origin;
    double rotationRadians;
  };

  struct Frames {
    RendererBitmap actual;
    RendererBitmap expected;
    RendererBitmap transparent;
  };

  void loadFont(FontManager& fonts) {
    const std::string path =
        Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts/MPLUS1p-Regular.ttf");
    std::ifstream file(path, std::ios::binary);
    ASSERT_THAT(file.is_open(), testing::IsTrue());
    bytes_ = std::make_shared<const std::vector<uint8_t>>(std::istreambuf_iterator<char>(file),
                                                          std::istreambuf_iterator<char>());
    ASSERT_THAT(*bytes_, testing::Not(testing::IsEmpty()));
    css::FontFaceSource source;
    source.kind = css::FontFaceSource::Kind::Data;
    source.payload = bytes_;
    source.trusted = true;
    css::FontFace face;
    face.familyName = "Path Precision";
    face.sources.push_back(std::move(source));
    fonts.addFontFace(face);
    font_ = fonts.findFont("Path Precision");
    ASSERT_THAT(font_, testing::Not(testing::Eq(FontHandle())));
    ASSERT_THAT(fonts.fontData(font_), testing::ElementsAreArray(*bytes_));
  }

  void expectFontMetrics(FontManager& fonts, size_t glyphCount) const {
    const auto head = fonts.sfntTable(font_, "head");
    const auto hhea = fonts.sfntTable(font_, "hhea");
    const auto vhea = fonts.sfntTable(font_, "vhea");
    const auto hmtx = fonts.sfntTable(font_, "hmtx");
    const auto vmtx = fonts.sfntTable(font_, "vmtx");
    const auto loca = fonts.sfntTable(font_, "loca");
    const auto glyf = fonts.sfntTable(font_, "glyf");
    ASSERT_THAT(head, testing::Optional(testing::SizeIs(testing::Ge(54))));
    ASSERT_THAT(hhea, testing::Optional(testing::SizeIs(testing::Ge(36))));
    ASSERT_THAT(vhea, testing::Optional(testing::SizeIs(testing::Ge(36))));
    ASSERT_THAT(hmtx, testing::Optional(testing::_));
    ASSERT_THAT(vmtx, testing::Optional(testing::_));
    ASSERT_THAT(loca, testing::Optional(testing::_));
    ASSERT_THAT(glyf, testing::Optional(testing::_));
    ASSERT_THAT(ReadUint16(*head, 18), testing::Eq(1000));
    const size_t horizontalMetrics = ReadUint16(*hhea, 34);
    const size_t verticalMetrics = ReadUint16(*vhea, 34);
    ASSERT_GT(horizontalMetrics, 0u);
    ASSERT_GT(verticalMetrics, 0u);
    const int16_t locaFormat = ReadInt16(*head, 50);
    ASSERT_THAT(locaFormat, testing::AnyOf(testing::Eq(0), testing::Eq(1)));
    for (size_t i = 0; i < glyphCount; ++i) {
      SCOPED_TRACE(kGlyphs[i]);
      const size_t glyph = kGlyphs[i];
      ASSERT_THAT(ReadUint16(*hmtx, 4 * std::min(glyph, horizontalMetrics - 1)), testing::Eq(1000));
      ASSERT_THAT(ReadUint16(*vmtx, 4 * std::min(glyph, verticalMetrics - 1)), testing::Eq(1000));
      const size_t bearingOffset = glyph < verticalMetrics
                                       ? 4 * glyph + 2
                                       : 4 * verticalMetrics + 2 * (glyph - verticalMetrics);
      const size_t glyphOffset =
          locaFormat == 0 ? 2 * ReadUint16(*loca, 2 * glyph) : ReadUint32(*loca, 4 * glyph);
      ASSERT_GE(glyf->size(), glyphOffset + 10);
      ASSERT_THAT(ReadInt16(*glyf, glyphOffset + 8) + ReadInt16(*vmtx, bearingOffset),
                  testing::Eq(860));
    }
  }

  void prepareIndependent(TextBackendFull& backend, std::string_view characters,
                          std::span<const IndependentFrame> frames) {
    TextBackend::ShapingContext::WorkBudget work{4096, 64};
    TextBackend::ShapingContext context;
    context.byteLength = characters.size();
    context.work = &work;
    const auto shaped =
        backend.shapeRunInContext(font_, 24, characters, context, 0, characters.size(), true,
                                  TextOrientation::Mixed, FontVariant::Normal, true);
    ASSERT_THAT(work.rejected, testing::IsFalse());
    ASSERT_THAT(shaped.glyphs, testing::SizeIs(frames.size()));
    for (size_t i = 0; i < frames.size(); ++i) {
      SCOPED_TRACE(i);
      const auto& glyph = shaped.glyphs[i];
      ASSERT_THAT(glyph.glyphIndex, testing::Eq(kGlyphs[i]));
      ASSERT_THAT(glyph.yAdvance, testing::DoubleEq(24));
      ASSERT_THAT(glyph.xOffset, testing::DoubleEq(-12));
      ASSERT_GT(glyph.yOffset, 0);
      Path outline =
          backend.glyphOutline(font_, glyph.glyphIndex, backend.scaleForEmToPixels(font_, 24));
      ASSERT_THAT(outline.commands(), testing::Not(testing::IsEmpty()));
      const auto& frame = frames[i];
      const Vector2d tangent(std::cos(frame.tangentRadians), std::sin(frame.tangentRadians));
      const Vector2d normal(-tangent.y, tangent.x);
      independent_.push_back(
          {std::move(outline),
           frame.midpoint + (glyph.yOffset - 12) * tangent - glyph.xOffset * normal,
           frame.tangentRadians - std::acos(-1.0) * 0.5});
    }
  }

  void expectPlacedOrigins(size_t glyphCount) {
    const auto text = actual_.querySelector("#text");
    ASSERT_THAT(text, testing::Optional(testing::_));
    ASSERT_THAT(text->cast<SVGTextElement>().getComputedTextLength(),
                testing::DoubleEq(24 * glyphCount));
    const auto& geometry = actual_.registry().get<components::ComputedTextGeometryComponent>(
        text->unsafeEntityHandle().entity());
    std::vector<TextGlyph> placed;
    for (const auto& run : geometry.runs) {
      if (!run.glyphs.empty()) {
        ASSERT_THAT(run.font, testing::Eq(font_));
        ASSERT_THAT(run.onPath, testing::IsTrue());
      }
      placed.insert(placed.end(), run.glyphs.begin(), run.glyphs.end());
    }
    ASSERT_THAT(placed, testing::SizeIs(glyphCount));
    for (size_t i = 0; i < placed.size(); ++i) {
      SCOPED_TRACE(i);
      EXPECT_THAT(placed[i].glyphIndex, testing::Eq(kGlyphs[i]));
      EXPECT_THAT(placed[i].font, testing::AnyOf(testing::Eq(FontHandle()), testing::Eq(font_)));
      EXPECT_THAT(Vector2d(placed[i].xPosition, placed[i].yPosition),
                  Vector2Eq(testing::DoubleNear(independent_[i].origin.x, 0.001),
                            testing::DoubleNear(independent_[i].origin.y, 0.001)));
      EXPECT_THAT(
          placed[i].rotateDegrees,
          testing::DoubleNear(independent_[i].rotationRadians * 180 / std::acos(-1.0), 0.001));
    }
  }

  void prepare(std::string_view route, std::string_view characters,
               std::span<const IndependentFrame> frames, bool centered = false) {
    ASSERT_LE(frames.size(), kGlyphs.size());
    actual_ = instantiateSubtree(
        "<svg viewBox='0 0 200 200'><path id='route' fill='none' d='" + std::string(route) +
            "'/><text id='text' font-family='Path Precision' font-size='24' writing-mode='tb' "
            "text-anchor='" +
            (centered ? "middle" : "start") + "'><textPath href='#route' startOffset='" +
            (centered ? "50%" : "0") + "'>" + std::string(characters) + "</textPath></text></svg>",
        {}, {500, 500});
    auto& registry = actual_.registry();
    auto& fonts = registry.ctx().emplace<FontManager>(registry);
    ASSERT_NO_FATAL_FAILURE(loadFont(fonts));
    ASSERT_NO_FATAL_FAILURE(expectFontMetrics(fonts, frames.size()));
    TextBackendFull backend(fonts, registry);
    ASSERT_NO_FATAL_FAILURE(prepareIndependent(backend, characters, frames));
    registry.ctx().emplace<TextEngine>(fonts, registry,
                                       std::make_unique<TextBackendFull>(fonts, registry));
    ASSERT_NO_FATAL_FAILURE(expectPlacedOrigins(frames.size()));
  }

  RendererBitmap renderIndependent() const {
    std::string source = "<svg viewBox='0 0 200 200'>";
    for (size_t i = 0; i < independent_.size(); ++i) {
      source += "<path id='glyph" + std::to_string(i) + "'/>";
    }
    source += "</svg>";
    auto document = instantiateSubtree(source, {}, {500, 500});
    for (size_t i = 0; i < independent_.size(); ++i) {
      const auto element = document.querySelector("#glyph" + std::to_string(i));
      EXPECT_THAT(element, testing::Optional(testing::_));
      if (element) {
        auto path = element->cast<SVGPathElement>();
        path.setSpline(independent_[i].outline.transformed(
            Transform2d::Rotate(independent_[i].rotationRadians)));
        path.setTransform(Transform2d::Translate(independent_[i].origin));
      }
    }
    return RenderDocumentWithActiveBackend(document);
  }

  Frames renderFrames() {
    auto empty = instantiateSubtree("<g/>", {}, {500, 500});
    return {RenderDocumentWithActiveBackend(actual_), renderIndependent(),
            RenderDocumentWithActiveBackend(empty)};
  }

  SVGDocument actual_;
  FontHandle font_;
  std::shared_ptr<const std::vector<uint8_t>> bytes_;
  std::vector<Occurrence> independent_;
};

TEST_F(VerticalTextPathPrecision, CurvedCjkUsesIndependentArcFrames) {
  ASSERT_NO_FATAL_FAILURE(prepare("M20 100C35 135 85 135 100 100C115 65 165 65 180 100",
                                  "日本語の表現", kCurvedFrames));
  const auto frames = renderFrames();
  EXPECT_NONFATAL_FAILURE(editor::tests::CompareBitmapToBitmap(
                              frames.expected, frames.transparent, "curved_path_precision_visible",
                              editor::tests::PixelmatchIdentityParams()),
                          "pixels differ");
  editor::tests::CompareBitmapToBitmap(frames.actual, frames.expected, "curved_path_precision",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(VerticalTextPathPrecision, AnalyticHorizontalTangentAtArcMidpoint) {
  const std::array<IndependentFrame, 1> expected{{{{100, 40}, 0}}};
  ASSERT_NO_FATAL_FAILURE(prepare("M20 100C20 20 180 20 180 100", "日", expected, true));
  const auto frames = renderFrames();
  EXPECT_NONFATAL_FAILURE(
      editor::tests::CompareBitmapToBitmap(frames.expected, frames.transparent,
                                           "horizontal_path_precision_visible",
                                           editor::tests::PixelmatchIdentityParams()),
      "pixels differ");
  editor::tests::CompareBitmapToBitmap(frames.actual, frames.expected, "horizontal_path_precision",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(VerticalTextPathPrecision, AnalyticVerticalTangentAtArcMidpoint) {
  const std::array<IndependentFrame, 1> expected{{{{160, 100}, std::acos(-1.0) * 0.5}}};
  ASSERT_NO_FATAL_FAILURE(prepare("M100 20C180 20 180 180 100 180", "日", expected, true));
  const auto frames = renderFrames();
  EXPECT_NONFATAL_FAILURE(
      editor::tests::CompareBitmapToBitmap(frames.expected, frames.transparent,
                                           "vertical_path_precision_visible",
                                           editor::tests::PixelmatchIdentityParams()),
      "pixels differ");
  editor::tests::CompareBitmapToBitmap(frames.actual, frames.expected, "vertical_path_precision",
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
