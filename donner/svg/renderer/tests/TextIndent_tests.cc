#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "donner/base/Utf8.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/css/FontFace.h"
#include "donner/svg/SVG.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/properties/PropertyRegistry.h"
#include "donner/svg/renderer/PlacedTextGeometry.h"
#include "donner/svg/renderer/RenderSnapshot.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/tests/MockRendererInterface.h"
#include "donner/svg/renderer/tests/MockTextBackend.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextEngine.h"
#include "donner/svg/text/TextLayoutParams.h"

namespace donner::svg {
namespace {

using testing::_;
using testing::DoubleNear;
using testing::ElementsAreArray;
using testing::Eq;
using testing::IsEmpty;
using testing::Not;
using testing::Optional;
using testing::Return;
using testing::SizeIs;

TextBackend::ShapedRun TenUnitShape(FontHandle, float, std::string_view text, size_t offset,
                                    size_t length, bool, FontVariant, bool) {
  TextBackend::ShapedRun result;
  const size_t end = offset + length;
  for (size_t byte = offset; byte < end;) {
    const size_t start = byte;
    const auto [codepoint, bytes] = Utf8::NextCodepointLenient(text.substr(byte));
    byte += static_cast<size_t>(bytes);
    TextBackend::ShapedGlyph glyph;
    glyph.glyphIndex = static_cast<int>(codepoint) + 1;
    glyph.cluster = static_cast<uint32_t>(start);
    glyph.xAdvance = 10;
    glyph.yAdvance = 10;
    result.glyphs.push_back(glyph);
  }
  return result;
}

std::unique_ptr<TextBackend> MakeIndentBackend(bool backward) {
  auto backend = std::make_unique<testing::NiceMock<MockTextBackend>>();
  ON_CALL(*backend, fontVMetrics(_))
      .WillByDefault(Return(FontVMetrics{
          .ascent = 1000, .descent = 0, .lineGap = 0, .xHeight = 500, .unitsPerEm = 1000}));
  ON_CALL(*backend, scaleForPixelHeight(_, _)).WillByDefault([](FontHandle, float size) {
    return size / 1000.0f;
  });
  ON_CALL(*backend, scaleForEmToPixels(_, _)).WillByDefault([](FontHandle, float size) {
    return size / 1000.0f;
  });
  ON_CALL(*backend, shapeRun(_, _, _, _, _, _, _, _))
      .WillByDefault([backward](FontHandle font, float size, std::string_view text, size_t offset,
                                size_t length, bool vertical, FontVariant variant, bool logical) {
        auto shaped = TenUnitShape(font, size, text, offset, length, vertical, variant, logical);
        shaped.backward = backward && !logical;
        if (*shaped.backward) {
          std::reverse(shaped.glyphs.begin(), shaped.glyphs.end());
        }
        return shaped;
      });
  ON_CALL(*backend, glyphOutline(_, _, _)).WillByDefault([](FontHandle, int glyph, float) {
    return glyph == 0 || glyph == ' ' + 1
               ? Path()
               : PathBuilder().addRect(Box2d::FromXYWH(0, -8, 8, 8)).build();
  });
  return backend;
}

std::string IndentScene(std::string_view content, std::string_view style,
                        std::string_view attributes = {}, std::string_view parentStyle = {}) {
  return "<svg xmlns='http://www.w3.org/2000/svg' width='400' height='400'>"
         "<path id='p' d='M0 100H500'/><g id='g' style='" +
         std::string(parentStyle) + "'><text id='t' x='100' y='100' " + std::string(attributes) +
         " style='font-family:IndentFace;font-size:20px;font-kerning:none;" + std::string(style) +
         "'>" + std::string(content) + "</text></g></svg>";
}

struct IndentObservation {
  std::vector<TextGlyph> glyphs;
  std::vector<uint32_t> codepoints;
  std::vector<float> usedSizes;
};

IndentObservation ReadIndentGeometry(EntityHandle handle, FontManager& fonts, TextEngine& engine,
                                     std::span<const uint8_t> bytes) {
  const auto& text = handle.get<components::ComputedTextComponent>();
  const auto& geometry = engine.ensureComputedTextGeometryComponent(handle);
  IndentObservation result;
  for (size_t ri = 0; ri < geometry.runs.size(); ++ri) {
    const auto& run = geometry.runs[ri];
    const auto& span = text.spans[ri];
    if (!run.glyphs.empty()) {
      EXPECT_THAT(fonts.fontData(run.font), ElementsAreArray(bytes));
    }
    const std::string_view sourceText(span.text.data() + span.start, span.end - span.start);
    for (const auto& glyph : run.glyphs) {
      const auto [codepoint, length] = Utf8::NextCodepointLenient(
          sourceText.substr(glyph.positioningCluster.value_or(glyph.cluster)));
      if (codepoint != '\n') {
        result.glyphs.push_back(glyph);
        result.codepoints.push_back(codepoint);
        result.usedSizes.push_back(run.usedFontSizePx);
      }
    }
  }
  return result;
}

IndentObservation ObserveIndent(std::string_view source, bool native = false,
                                std::string_view filename = "NotoSans-Regular.ttf",
                                const std::function<void(SVGDocument&)>& mutate = {},
                                IndentObservation* before = nullptr, bool backward = false) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  EXPECT_THAT(parsed, NoParseError());
  if (parsed.hasError()) {
    return {};
  }
  SVGDocument document = std::move(parsed).result();
  const auto root = document.querySelector("#t");
  EXPECT_THAT(root, Optional(_));
  if (!root) {
    return {};
  }
  const std::string fontPath =
      Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts/" + std::string(filename));
  std::ifstream input(fontPath, std::ios::binary);
  const auto bytes = std::make_shared<const std::vector<uint8_t>>(
      std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  EXPECT_THAT(*bytes, Not(IsEmpty()));
  if (bytes->empty()) {
    return {};
  }
  Registry& registry = document.registry();
  FontManager fonts(registry);
  css::FontFace face;
  face.familyName = "IndentFace";
  face.sources.push_back({css::FontFaceSource::Kind::Data, bytes, {}, {}, true});
  fonts.addFontFace(face);
  auto engine = native ? std::make_unique<TextEngine>(fonts, registry)
                       : std::make_unique<TextEngine>(fonts, registry, MakeIndentBackend(backward));
  const auto handle = root->unsafeEntityHandle();
  engine->prepareForElement(handle, warnings);
  if (before) {
    *before = ReadIndentGeometry(handle, fonts, *engine, *bytes);
  }
  if (mutate) {
    mutate(document);
    engine->prepareForElement(handle, warnings);
  }
  return ReadIndentGeometry(handle, fonts, *engine, *bytes);
}

double InlinePen(const TextGlyph& glyph, WritingMode mode) {
  return mode == WritingMode::HorizontalTb ? glyph.xPosition
                                           : glyph.yPosition - glyph.verticalOriginOffset;
}

double BlockPen(const TextGlyph& glyph, WritingMode mode) {
  return mode == WritingMode::HorizontalTb ? glyph.yPosition : glyph.xPosition;
}

std::string WritingStyle(WritingMode mode) {
  switch (mode) {
    case WritingMode::HorizontalTb: return "writing-mode:horizontal-tb;";
    case WritingMode::VerticalRl: return "writing-mode:vertical-rl;";
    case WritingMode::VerticalLr: return "writing-mode:vertical-lr;";
  }
  return {};
}

std::string AnchorStyle(TextAnchor anchor) {
  switch (anchor) {
    case TextAnchor::Start: return "text-anchor:start;";
    case TextAnchor::Middle: return "text-anchor:middle;";
    case TextAnchor::End: return "text-anchor:end;";
  }
  return {};
}

using AnchorCase = std::tuple<WritingMode, TextAnchor, double>;
class TextIndentAnchorTest : public testing::TestWithParam<AnchorCase> {};

TEST_P(TextIndentAnchorTest, FirstLineMarginParticipatesInPerLineAnchor) {
  const auto [mode, anchor, indent] = GetParam();
  const std::string style =
      WritingStyle(mode) + AnchorStyle(anchor) +
      "white-space:pre;inline-size:100px;text-indent:" + std::to_string(indent) + "px;";
  const auto observed = ObserveIndent(IndentScene("AB", style));
  ASSERT_THAT(observed.glyphs, SizeIs(2));
  const double baseline = anchor == TextAnchor::Start    ? 100
                          : anchor == TextAnchor::Middle ? 90
                                                         : 80;
  const double contribution = anchor == TextAnchor::Start    ? indent
                              : anchor == TextAnchor::Middle ? indent * 0.5
                                                             : 0;
  EXPECT_THAT(InlinePen(observed.glyphs[0], mode), DoubleNear(baseline + contribution, 1e-5));
  EXPECT_THAT(InlinePen(observed.glyphs[1], mode), DoubleNear(baseline + contribution + 10, 1e-5));
  EXPECT_THAT(
      mode == WritingMode::HorizontalTb ? observed.glyphs[0].xAdvance : observed.glyphs[0].yAdvance,
      DoubleNear(10, 1e-5));
}

INSTANTIATE_TEST_SUITE_P(
    AllAxes, TextIndentAnchorTest,
    testing::Combine(testing::Values(WritingMode::HorizontalTb, WritingMode::VerticalRl,
                                     WritingMode::VerticalLr),
                     testing::Values(TextAnchor::Start, TextAnchor::Middle, TextAnchor::End),
                     testing::Values(0.0, 10.0, -10.0)));

class TextIndentAxisTest : public testing::TestWithParam<WritingMode> {};

TEST_P(TextIndentAxisTest, FirstLineFitChangesWithoutTranslatingLaterLine) {
  const WritingMode mode = GetParam();
  const auto control =
      ObserveIndent(IndentScene("A B C", WritingStyle(mode) + "inline-size:45px;text-indent:0;"));
  const auto observed = ObserveIndent(
      IndentScene("A B C", WritingStyle(mode) + "inline-size:45px;text-indent:20px;"));
  ASSERT_THAT(control.glyphs, SizeIs(5));
  ASSERT_THAT(observed.glyphs, SizeIs(5));
  const double step = mode == WritingMode::VerticalRl ? -20 : 20;
  EXPECT_THAT(InlinePen(control.glyphs[0], mode), DoubleNear(100, 1e-5));
  EXPECT_THAT(InlinePen(control.glyphs[2], mode), DoubleNear(120, 1e-5));
  EXPECT_THAT(BlockPen(control.glyphs[2], mode),
              DoubleNear(BlockPen(control.glyphs[0], mode), 1e-5));
  EXPECT_THAT(BlockPen(control.glyphs[4], mode) - BlockPen(control.glyphs[0], mode),
              DoubleNear(step, 1e-5));
  EXPECT_THAT(InlinePen(observed.glyphs[0], mode), DoubleNear(120, 1e-5));
  EXPECT_THAT(InlinePen(observed.glyphs[2], mode), DoubleNear(100, 1e-5));
  EXPECT_THAT(InlinePen(observed.glyphs[4], mode), DoubleNear(120, 1e-5));
  EXPECT_THAT(BlockPen(observed.glyphs[2], mode) - BlockPen(observed.glyphs[0], mode),
              DoubleNear(step, 1e-5));
}

TEST_P(TextIndentAxisTest, ForcedBreakDoesNotRestartPlainIndent) {
  const WritingMode mode = GetParam();
  const auto observed = ObserveIndent(IndentScene(
      "A&#10;B", WritingStyle(mode) + "white-space:pre;inline-size:100px;text-indent:10px;"));
  ASSERT_THAT(observed.glyphs, SizeIs(2));
  EXPECT_THAT(InlinePen(observed.glyphs[0], mode), DoubleNear(110, 1e-5));
  EXPECT_THAT(InlinePen(observed.glyphs[1], mode), DoubleNear(100, 1e-5));
}

TEST_P(TextIndentAxisTest, EmptyForcedFirstLineConsumesIndentButEmptySpanDoesNot) {
  const WritingMode mode = GetParam();
  const std::string style =
      WritingStyle(mode) + "white-space:pre;inline-size:100px;text-indent:10px;";
  const auto forced = ObserveIndent(IndentScene("&#10;A", style));
  const auto emptySpan = ObserveIndent(IndentScene("<tspan/>A", style));
  ASSERT_THAT(forced.glyphs, SizeIs(1));
  ASSERT_THAT(emptySpan.glyphs, SizeIs(1));
  EXPECT_THAT(InlinePen(forced.glyphs[0], mode), DoubleNear(100, 1e-5));
  EXPECT_THAT(InlinePen(emptySpan.glyphs[0], mode), DoubleNear(110, 1e-5));
}

TEST_P(TextIndentAxisTest, PositiveIndentWiderThanAreaKeepsTheFirstUnit) {
  const WritingMode mode = GetParam();
  const auto control =
      ObserveIndent(IndentScene("A B", WritingStyle(mode) + "inline-size:20px;text-indent:0;"));
  const auto observed =
      ObserveIndent(IndentScene("A B", WritingStyle(mode) + "inline-size:20px;text-indent:40px;"));
  ASSERT_THAT(control.glyphs, SizeIs(3));
  ASSERT_THAT(observed.glyphs, SizeIs(3));
  const double initialBlock = mode == WritingMode::HorizontalTb ? 100 : 90;
  const double lineAdvance = mode == WritingMode::VerticalRl ? -20 : 20;
  EXPECT_THAT(InlinePen(control.glyphs[0], mode), DoubleNear(100, 1e-5));
  EXPECT_THAT(BlockPen(control.glyphs[0], mode), DoubleNear(initialBlock, 1e-5));
  EXPECT_THAT(InlinePen(observed.glyphs[0], mode), DoubleNear(140, 1e-5));
  EXPECT_THAT(InlinePen(observed.glyphs[2], mode), DoubleNear(100, 1e-5));
  EXPECT_THAT(BlockPen(observed.glyphs[0], mode),
              DoubleNear(BlockPen(control.glyphs[0], mode), 1e-5));
  EXPECT_THAT(BlockPen(observed.glyphs[2], mode) - BlockPen(observed.glyphs[0], mode),
              DoubleNear(lineAdvance, 1e-5));
}

TEST_P(TextIndentAxisTest, NegativeIndentExpandsOnlyFirstLineCapacity) {
  const WritingMode mode = GetParam();
  const auto observed = ObserveIndent(
      IndentScene("A B C", WritingStyle(mode) + "inline-size:40px;text-indent:-10px;"));
  ASSERT_THAT(observed.glyphs, SizeIs(5));
  EXPECT_THAT(InlinePen(observed.glyphs[0], mode), DoubleNear(90, 1e-5));
  EXPECT_THAT(InlinePen(observed.glyphs[4], mode), DoubleNear(130, 1e-5));
  EXPECT_THAT(BlockPen(observed.glyphs[4], mode),
              DoubleNear(BlockPen(observed.glyphs[0], mode), 1e-5));
}

INSTANTIATE_TEST_SUITE_P(AllAxes, TextIndentAxisTest,
                         testing::Values(WritingMode::HorizontalTb, WritingMode::VerticalRl,
                                         WritingMode::VerticalLr));

TEST(TextIndentTest, PercentageUsesOwnInlineMeasureAndInlineSpanCannotOverrideBlock) {
  const auto observed = ObserveIndent(IndentScene("<tspan style='text-indent:90px'>AB</tspan>",
                                                  "inline-size:100px;", {}, "text-indent:10%;"));
  ASSERT_THAT(observed.glyphs, SizeIs(2));
  EXPECT_THAT(observed.glyphs[0].xPosition, DoubleNear(110, 1e-5));
}

TEST(TextIndentTest, InheritedEmUsesDeclaringComputedFontSize) {
  const auto observed = ObserveIndent(IndentScene("AB", "inline-size:100px;font-size:20px;", {},
                                                  "font-size:10px;text-indent:2em;"));
  ASSERT_THAT(observed.glyphs, SizeIs(2));
  EXPECT_THAT(observed.glyphs[0].xPosition, DoubleNear(120, 1e-5));
  EXPECT_THAT(observed.usedSizes, testing::Each(Eq(20.0f)));
}

TEST(TextIndentTest, FontSizeAdjustDoesNotScaleComputedEmIndent) {
  const auto observed = ObserveIndent(
      IndentScene("AB", "inline-size:100px;font-size:20px;font-size-adjust:1;text-indent:1em;"));
  ASSERT_THAT(observed.glyphs, SizeIs(2));
  EXPECT_THAT(observed.usedSizes, testing::Each(Eq(40.0f)));
  EXPECT_THAT(observed.glyphs[0].xPosition, DoubleNear(120, 1e-5));
}

TEST(TextIndentTest, TabStopsSeeCssIndentBeforeSvgRelativePosition) {
  const auto observed = ObserveIndent(
      IndentScene("A&#9;B", "white-space:pre;inline-size:400px;text-indent:67.5px;", "dx='20'"));
  ASSERT_THAT(observed.glyphs, SizeIs(3));
  EXPECT_THAT(observed.glyphs[1].xAdvance, DoubleNear(82.5, 1e-5));
  EXPECT_THAT(observed.glyphs[2].xPosition, DoubleNear(280, 1e-5));
}

TEST(TextIndentTest, NoWrappingAreaAndTextPathRetainTheirExistingFlow) {
  const auto unwrapped = ObserveIndent(IndentScene("AB", "text-indent:10px;"));
  const auto path = ObserveIndent(
      IndentScene("<textPath href='#p'>AB</textPath>", "inline-size:100px;text-indent:10px;"));
  const auto pathControl = ObserveIndent(
      IndentScene("<textPath href='#p'>AB</textPath>", "inline-size:100px;text-indent:0;"));
  ASSERT_THAT(unwrapped.glyphs, SizeIs(2));
  ASSERT_THAT(path.glyphs, SizeIs(2));
  ASSERT_THAT(pathControl.glyphs, SizeIs(2));
  EXPECT_THAT(unwrapped.glyphs[0].xPosition, DoubleNear(100, 1e-5));
  EXPECT_THAT(path.glyphs[0].xPosition, Eq(pathControl.glyphs[0].xPosition));
  EXPECT_THAT(path.glyphs[1].xPosition, Eq(pathControl.glyphs[1].xPosition));
}

TEST(TextIndentTest, DefaultComputedFontSizeFeedsLocalEm) {
  const auto observed =
      ObserveIndent(IndentScene("AB", "font-size:initial;inline-size:100px;text-indent:1em;"));
  ASSERT_THAT(observed.glyphs, SizeIs(2));
  EXPECT_THAT(observed.usedSizes, testing::Each(Eq(12.0f)));
  EXPECT_THAT(observed.glyphs[0].xPosition, DoubleNear(112, 1e-5));
}

TEST(TextIndentTest, NestedRelativeFontsDoNotReinterpretInheritedEm) {
  const auto observed = ObserveIndent(IndentScene("<tspan style='font-size:2em'>AB</tspan>",
                                                  "font-size:2em;inline-size:100px;", {},
                                                  "font-size:10px;text-indent:2em;"));
  ASSERT_THAT(observed.glyphs, SizeIs(2));
  EXPECT_THAT(observed.usedSizes, testing::Each(Eq(40.0f)));
  EXPECT_THAT(observed.glyphs[0].xPosition, DoubleNear(120, 1e-5));
}

TEST(TextIndentTest, ParentFontMutationRecomputesInheritedEmAndCachedGeometry) {
  IndentObservation before;
  const auto after = ObserveIndent(
      IndentScene("AB", "inline-size:100px;", {}, "font-size:10px;text-indent:2em;"), false,
      "NotoSans-Regular.ttf",
      [](SVGDocument& document) {
        auto parent = document.querySelector("#g");
        ASSERT_THAT(parent, Optional(_));
        parent->setAttribute("style", "font-size:20px;text-indent:2em;");
      },
      &before);
  ASSERT_THAT(before.glyphs, SizeIs(2));
  ASSERT_THAT(after.glyphs, SizeIs(2));
  EXPECT_THAT(before.glyphs[0].xPosition, DoubleNear(120, 1e-5));
  EXPECT_THAT(after.glyphs[0].xPosition, DoubleNear(140, 1e-5));
}

TEST(TextIndentTest, ChildFontMutationKeepsInheritedComputedEmLength) {
  IndentObservation before;
  const auto after = ObserveIndent(
      IndentScene("AB", "inline-size:100px;", {}, "font-size:10px;text-indent:2em;"), false,
      "NotoSans-Regular.ttf",
      [](SVGDocument& document) {
        auto root = document.querySelector("#t");
        ASSERT_THAT(root, Optional(_));
        root->setAttribute("style", "font-family:IndentFace;font-size:40px;inline-size:100px;");
      },
      &before);
  ASSERT_THAT(before.glyphs, SizeIs(2));
  ASSERT_THAT(after.glyphs, SizeIs(2));
  EXPECT_THAT(before.glyphs[0].xPosition, DoubleNear(120, 1e-5));
  EXPECT_THAT(after.glyphs[0].xPosition, DoubleNear(120, 1e-5));
  EXPECT_THAT(after.usedSizes, testing::Each(Eq(40.0f)));
}

TEST(TextIndentTest, WidthMutationResolvesInheritedPercentAgainstOwnArea) {
  IndentObservation before;
  const auto after = ObserveIndent(
      IndentScene("AB", "inline-size:100px;", {}, "text-indent:10%;"), false,
      "NotoSans-Regular.ttf",
      [](SVGDocument& document) {
        auto root = document.querySelector("#t");
        ASSERT_THAT(root, Optional(_));
        root->setAttribute("style", "font-family:IndentFace;font-size:20px;inline-size:200px;");
      },
      &before);
  ASSERT_THAT(before.glyphs, SizeIs(2));
  ASSERT_THAT(after.glyphs, SizeIs(2));
  EXPECT_THAT(before.glyphs[0].xPosition, DoubleNear(110, 1e-5));
  EXPECT_THAT(after.glyphs[0].xPosition, DoubleNear(120, 1e-5));
}

TEST(TextIndentCompatibilityTest, LegacyPositionalLayoutKeepsInlineMeasure) {
  TextLayoutParams legacy{{},
                          Lengthd(20, Lengthd::Unit::Px),
                          Box2d::FromXYWH(0, 0, 400, 400),
                          FontMetrics(),
                          TextAnchor::Start,
                          WritingMode::HorizontalTb,
                          FontKerning::Auto,
                          std::nullopt,
                          0,
                          0,
                          std::nullopt,
                          LengthAdjust::Default,
                          100};
  EXPECT_THAT(legacy.inlineSizePx, Eq(100.0));
  EXPECT_THAT(legacy.textLength, Eq(std::nullopt));
}

class TextIndentLengthTest : public testing::TestWithParam<bool> {};

TEST_P(TextIndentLengthTest, GlobalAndSpanTextLengthDoNotScaleLeadingMargin) {
  const std::string content = GetParam() ? "<tspan textLength='40'>AB</tspan>" : "AB";
  const std::string attributes = GetParam() ? "" : "textLength='40'";
  const auto control =
      ObserveIndent(IndentScene(content, "inline-size:100px;text-indent:0;", attributes));
  const auto observed =
      ObserveIndent(IndentScene(content, "inline-size:100px;text-indent:10px;", attributes));
  ASSERT_THAT(control.glyphs, SizeIs(2));
  ASSERT_THAT(observed.glyphs, SizeIs(2));
  EXPECT_THAT(control.glyphs[0].xPosition, DoubleNear(100, 1e-5));
  EXPECT_THAT(control.glyphs[1].xPosition, DoubleNear(130, 1e-5));
  EXPECT_THAT(observed.glyphs[0].xPosition, DoubleNear(110, 1e-5));
  EXPECT_THAT(observed.glyphs[1].xPosition, DoubleNear(140, 1e-5));
  EXPECT_THAT(observed.glyphs[0].xAdvance, Eq(control.glyphs[0].xAdvance));
}

INSTANTIATE_TEST_SUITE_P(Owner, TextIndentLengthTest, testing::Values(false, true));

TEST(TextIndentTest, NoWrapStillUsesTheExplicitContentArea) {
  const auto observed =
      ObserveIndent(IndentScene("AB", "white-space:nowrap;inline-size:5px;text-indent:10px;"));
  ASSERT_THAT(observed.glyphs, SizeIs(2));
  EXPECT_THAT(observed.glyphs[0].xPosition, DoubleNear(110, 1e-5));
  EXPECT_THAT(observed.glyphs[1].xPosition, DoubleNear(120, 1e-5));
  EXPECT_THAT(observed.glyphs[0].yPosition, Eq(observed.glyphs[1].yPosition));
}

TEST(TextIndentTest, EmergencyPlannerKeepsBackwardLogicalPrefixOnIndentedFirstLine) {
  const auto observed = ObserveIndent(
      IndentScene("ABCDE", "inline-size:35px;text-indent:10px;overflow-wrap:anywhere;"), false,
      "NotoSans-Regular.ttf", {}, nullptr, true);
  ASSERT_THAT(observed.glyphs, SizeIs(5));
  std::vector<uint32_t> clusters;
  for (const auto& glyph : observed.glyphs) {
    clusters.push_back(glyph.cluster);
  }
  EXPECT_THAT(clusters, testing::ElementsAre(1u, 0u, 4u, 3u, 2u));
  EXPECT_THAT(observed.glyphs[0].xPosition, DoubleNear(110, 1e-5));
  EXPECT_THAT(observed.glyphs[1].xPosition, DoubleNear(120, 1e-5));
  EXPECT_THAT(observed.glyphs[2].xPosition, DoubleNear(100, 1e-5));
  EXPECT_THAT(observed.glyphs[2].yPosition - observed.glyphs[0].yPosition, DoubleNear(20, 1e-5));
}

TEST(TextIndentTest, NativeBackwardShapingKeepsIdentityUnderExistingBaseProgression) {
  const std::string content = "\xD9\x86\xD9\x88\xD8\xB4\xD8\xAA\xD9\x86";
  const auto control =
      ObserveIndent(IndentScene(content, "inline-size:400px;text-indent:0;overflow-wrap:anywhere;"),
                    true, "Amiri-Regular.ttf");
  const auto observed = ObserveIndent(
      IndentScene(content, "inline-size:400px;text-indent:20px;overflow-wrap:anywhere;"), true,
      "Amiri-Regular.ttf");
  ASSERT_THAT(control.glyphs, SizeIs(testing::Gt(1u)));
  ASSERT_THAT(observed.glyphs, SizeIs(control.glyphs.size()));
#ifdef DONNER_TEXT_FULL
  EXPECT_THAT(control.glyphs.front().cluster, testing::Gt(control.glyphs.back().cluster));
#endif
  for (size_t index = 0; index < control.glyphs.size(); ++index) {
    EXPECT_THAT(observed.glyphs[index].xPosition,
                DoubleNear(control.glyphs[index].xPosition + 20, 1e-5));
    EXPECT_THAT(observed.glyphs[index].glyphIndex, Eq(control.glyphs[index].glyphIndex));
    EXPECT_THAT(observed.glyphs[index].cluster, Eq(control.glyphs[index].cluster));
    EXPECT_THAT(observed.glyphs[index].xAdvance, Eq(control.glyphs[index].xAdvance));
  }
}

TEST(TextIndentTest, NativeBackendPreservesAdvancesAndSourceClusters) {
  const auto control = ObserveIndent(IndentScene("AB", "inline-size:200px;text-indent:0;"), true);
  const auto observed =
      ObserveIndent(IndentScene("AB", "inline-size:200px;text-indent:20px;"), true);
  ASSERT_THAT(control.glyphs, SizeIs(2));
  ASSERT_THAT(observed.glyphs, SizeIs(2));
  for (size_t index = 0; index < 2; ++index) {
    EXPECT_THAT(observed.glyphs[index].xPosition,
                DoubleNear(control.glyphs[index].xPosition + 20, 1e-5));
    EXPECT_THAT(observed.glyphs[index].xAdvance, Eq(control.glyphs[index].xAdvance));
    EXPECT_THAT(observed.glyphs[index].cluster, Eq(control.glyphs[index].cluster));
    EXPECT_THAT(observed.glyphs[index].glyphIndex, Eq(control.glyphs[index].glyphIndex));
  }
}

using InheritedIndentCase = std::pair<const char*, double>;
class TextIndentInheritanceTest : public testing::TestWithParam<InheritedIndentCase> {};

TEST_P(TextIndentInheritanceTest, LocalCascadePrecedesParentSpecificity) {
  const auto [local, expected] = GetParam();
  const auto observed = ObserveIndent(IndentScene("AB", std::string("inline-size:100px;") + local,
                                                  {}, "text-indent:30px!important;"));
  ASSERT_THAT(observed.glyphs, SizeIs(2));
  EXPECT_THAT(observed.glyphs[0].xPosition, DoubleNear(100 + expected, 1e-5));
}

INSTANTIATE_TEST_SUITE_P(CssWide, TextIndentInheritanceTest,
                         testing::Values(InheritedIndentCase{"text-indent:10px", 10},
                                         InheritedIndentCase{"text-indent:initial", 0},
                                         InheritedIndentCase{"text-indent:unset", 30},
                                         InheritedIndentCase{"text-indent:inherit", 30},
                                         InheritedIndentCase{"text-indent:10px!important", 10},
                                         InheritedIndentCase{"", 30}));

TEST_P(TextIndentAxisTest, LaterAbsoluteInlinePositionDoesNotRestartMargin) {
  const WritingMode mode = GetParam();
  const std::string coordinate = mode == WritingMode::HorizontalTb ? "x" : "y";
  const auto observed = ObserveIndent(
      IndentScene("A<tspan " + coordinate + "='300'>B&#10;C</tspan>",
                  WritingStyle(mode) + "white-space:pre;inline-size:100px;text-indent:20px;"));
  ASSERT_THAT(observed.glyphs, SizeIs(3));
  EXPECT_THAT(InlinePen(observed.glyphs[0], mode), DoubleNear(120, 1e-5));
  EXPECT_THAT(InlinePen(observed.glyphs[1], mode), DoubleNear(300, 1e-5));
  EXPECT_THAT(InlinePen(observed.glyphs[2], mode), DoubleNear(300, 1e-5));
}

TEST_P(TextIndentAxisTest, RelativePositionAfterBreakStillAccumulatesWithoutMargin) {
  const WritingMode mode = GetParam();
  const std::string coordinate = mode == WritingMode::HorizontalTb ? "dx" : "dy";
  const auto observed = ObserveIndent(
      IndentScene("A&#10;<tspan " + coordinate + "='20'>B</tspan>C",
                  WritingStyle(mode) + "white-space:pre;inline-size:100px;text-indent:20px;"));
  ASSERT_THAT(observed.glyphs, SizeIs(3));
  EXPECT_THAT(InlinePen(observed.glyphs[0], mode), DoubleNear(120, 1e-5));
  EXPECT_THAT(InlinePen(observed.glyphs[1], mode), DoubleNear(120, 1e-5));
  EXPECT_THAT(InlinePen(observed.glyphs[2], mode), DoubleNear(130, 1e-5));
}

TEST(TextIndentTest, OptionalLigatureChangesOnlyWhenFirstWordNeedsEmergencyCut) {
  const auto natural =
      ObserveIndent(IndentScene("ffi", "inline-size:200px;"), true, "NotoSerif-Regular.ttf");
  ASSERT_THAT(natural.glyphs, Not(IsEmpty()));
  double naturalWidth = 0;
  for (const auto& glyph : natural.glyphs) {
    naturalWidth += glyph.xAdvance;
  }
  const double measure = naturalWidth + 3;
  const auto observed = ObserveIndent(
      IndentScene("ffi ffi", "overflow-wrap:anywhere;inline-size:" + std::to_string(measure) +
                                 "px;text-indent:" + std::to_string(naturalWidth * 0.5) + "px;"),
      true, "NotoSerif-Regular.ttf");
  const auto first = std::find_if(observed.glyphs.begin(), observed.glyphs.end(),
                                  [](const auto& glyph) { return glyph.cluster == 0; });
  ASSERT_NE(first, observed.glyphs.end());
  EXPECT_THAT(first->xPosition, DoubleNear(100 + naturalWidth * 0.5, 1e-5));
  const auto secondWord = std::find_if(observed.glyphs.begin(), observed.glyphs.end(),
                                       [](const auto& glyph) { return glyph.cluster >= 4; });
  ASSERT_NE(secondWord, observed.glyphs.end());
  ASSERT_EQ(static_cast<size_t>(observed.glyphs.end() - secondWord), natural.glyphs.size());
  for (size_t index = 0; index < natural.glyphs.size(); ++index) {
    EXPECT_EQ(secondWord[index].glyphIndex, natural.glyphs[index].glyphIndex);
    EXPECT_EQ(secondWord[index].cluster, natural.glyphs[index].cluster + 4);
    EXPECT_THAT(secondWord[index].xAdvance, DoubleNear(natural.glyphs[index].xAdvance, 1e-5));
  }
#ifdef DONNER_TEXT_FULL
  ASSERT_THAT(natural.glyphs, SizeIs(1));
  EXPECT_GT(static_cast<size_t>(secondWord - first), natural.glyphs.size());
#endif
}

TEST(TextIndentTest, ContinuedWordUsesIndentedFirstLineForEmergencyPreflight) {
  const auto ligature =
      ObserveIndent(IndentScene("ffi", "inline-size:200px;"), true, "NotoSerif-Regular.ttf");
  const auto letter =
      ObserveIndent(IndentScene("A", "inline-size:200px;"), true, "NotoSerif-Regular.ttf");
  ASSERT_THAT(ligature.glyphs, Not(IsEmpty()));
  ASSERT_THAT(letter.glyphs, SizeIs(1));
  double ligatureWidth = 0;
  for (const auto& glyph : ligature.glyphs) {
    ligatureWidth += glyph.xAdvance;
  }
  const double wordWidth = letter.glyphs.front().xAdvance + ligatureWidth;
  // The word fits the full measure but not the first line after its indentation.
  const double measure = wordWidth + 3;
  const double indent = wordWidth * 0.5;
  const auto observed =
      ObserveIndent(IndentScene("A<tspan>ffi</tspan>",
                                "overflow-wrap:anywhere;inline-size:" + std::to_string(measure) +
                                    "px;text-indent:" + std::to_string(indent) + "px;"),
                    true, "NotoSerif-Regular.ttf");
  ASSERT_THAT(observed.glyphs, Not(IsEmpty()));
  EXPECT_THAT(observed.glyphs.front().xPosition, DoubleNear(100 + indent, 1e-5));
#ifdef DONNER_TEXT_FULL
  // The continuing span exposes its typographic units instead of keeping the ligature whole.
  ASSERT_THAT(ligature.glyphs, SizeIs(1));
  EXPECT_GT(observed.glyphs.size() - 1, ligature.glyphs.size());
#endif
}

void WithNativeIndentDocument(
    std::string_view source,
    const std::function<void(SVGDocument&, const css::FontFace&)>& inspect) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  ASSERT_THAT(parsed, NoParseError());
  SVGDocument document = std::move(parsed).result();
  const std::string path =
      Runfiles::instance().Rlocation("third_party/resvg-test-suite/fonts/NotoSans-Regular.ttf");
  std::ifstream input(path, std::ios::binary);
  const auto bytes = std::make_shared<const std::vector<uint8_t>>(
      std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  ASSERT_THAT(*bytes, Not(IsEmpty()));
  css::FontFace face;
  face.familyName = "IndentFace";
  face.sources.push_back({css::FontFaceSource::Kind::Data, bytes, {}, {}, true});
  FontManager fonts(document.registry());
  fonts.addFontFace(face);
  inspect(document, face);
}

std::vector<TextGlyph> DrawIndentGlyphs(Registry& registry,
                                        const components::ComputedTextComponent& text,
                                        const TextParams& params) {
  FontManager fonts(registry);
  TextEngine engine(fonts, registry);
  engine.addFontFaces(params.fontFaces);
  const auto geometry = PrepareTextDrawGeometry(registry, text, params, engine);
  std::vector<TextGlyph> result;
  for (const auto& run : geometry.runs) {
    result.insert(result.end(), run.glyphs.begin(), run.glyphs.end());
  }
  return result;
}

void ExpectSameIndentGlyphs(const std::vector<TextGlyph>& actual,
                            const std::vector<TextGlyph>& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t index = 0; index < actual.size(); ++index) {
    EXPECT_EQ(actual[index].glyphIndex, expected[index].glyphIndex);
    EXPECT_EQ(actual[index].cluster, expected[index].cluster);
    EXPECT_THAT(actual[index].xPosition, DoubleNear(expected[index].xPosition, 1e-5));
    EXPECT_THAT(actual[index].yPosition, DoubleNear(expected[index].yPosition, 1e-5));
    EXPECT_THAT(actual[index].xAdvance, DoubleNear(expected[index].xAdvance, 1e-5));
    EXPECT_THAT(actual[index].yAdvance, DoubleNear(expected[index].yAdvance, 1e-5));
  }
}

TEST(TextIndentConsumerTest, DriverCachedAndDirectFallbackUseTheSameComputedMargin) {
  WithNativeIndentDocument(
      IndentScene("AB", "white-space:pre;inline-size:100px;text-indent:20%;"),
      [](SVGDocument& document, const css::FontFace& face) {
        testing::NiceMock<tests::MockRendererInterface> renderer;
        RendererDriver driver(renderer);
        EXPECT_CALL(renderer, drawText(_, _, _))
            .WillOnce([&](Registry& registry, const components::ComputedTextComponent& text,
                          const TextParams& params) {
              const auto cached = DrawIndentGlyphs(registry, text, params);
              ASSERT_THAT(cached, SizeIs(2));
              EXPECT_THAT(cached.front().xPosition, DoubleNear(120, 1e-5));
              TextParams direct = params;
              direct.preparedTextDraw.reset();
              direct.textRootEntity = entt::null;
              direct.fontFaces = std::span<const css::FontFace>(&face, 1);
              ExpectSameIndentGlyphs(DrawIndentGlyphs(registry, text, direct), cached);
            });
        driver.draw(document);
      });
}

TEST(TextIndentConsumerTest, SnapshotUnpreparedFallbackRetainsMarginAfterSourceDestruction) {
  RenderSnapshot snapshot;
  std::vector<TextGlyph> expected;
  testing::NiceMock<tests::MockRendererInterface> renderer;
  WithNativeIndentDocument(
      IndentScene("AB", "white-space:pre;inline-size:100px;text-indent:20%;"),
      [&](SVGDocument& document, const css::FontFace& face) {
        RendererDriver driver(renderer);
        RenderSnapshotRecorder recorder(snapshot, renderer);
        EXPECT_CALL(renderer, drawText(_, _, _))
            .WillOnce([&](Registry& registry, const components::ComputedTextComponent& text,
                          const TextParams& params) {
              expected = DrawIndentGlyphs(registry, text, params);
              ASSERT_THAT(expected, SizeIs(2));
              EXPECT_THAT(expected.front().xPosition, DoubleNear(120, 1e-5));
              TextParams direct = params;
              direct.preparedTextDraw.reset();
              direct.textRootEntity = entt::null;
              direct.fontFaces = std::span<const css::FontFace>(&face, 1);
              recorder.drawText(registry, text, direct);
            });
        driver.draw(document);
      });
  ASSERT_GT(snapshot.commandCount(), 0u);
  EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(&renderer));
  EXPECT_CALL(renderer, drawText(_, _, _))
      .WillOnce([&](Registry& registry, const components::ComputedTextComponent& text,
                    const TextParams& params) {
        EXPECT_EQ(params.preparedTextDraw, nullptr);
        ExpectSameIndentGlyphs(DrawIndentGlyphs(registry, text, params), expected);
      });
  snapshot.replay(renderer);
}

TEST(TextIndentConsumerTest, NonfiniteUsedMarginRejectsOnlyItsAnchoredFirstLine) {
  WithNativeIndentDocument(
      IndentScene("A&#10;B", "white-space:pre;inline-size:100px;"),
      [](SVGDocument& document, const css::FontFace& face) {
        testing::NiceMock<tests::MockRendererInterface> renderer;
        RendererDriver driver(renderer);
        EXPECT_CALL(renderer, drawText(_, _, _))
            .WillOnce([&](Registry& registry, const components::ComputedTextComponent& text,
                          const TextParams& params) {
              TextParams direct = params;
              direct.preparedTextDraw.reset();
              direct.textRootEntity = entt::null;
              direct.fontFaces = std::span<const css::FontFace>(&face, 1);
              direct.textIndentPx = std::numeric_limits<double>::infinity();
              const auto glyphs = DrawIndentGlyphs(registry, text, direct);
              ASSERT_THAT(glyphs, SizeIs(1));
              EXPECT_EQ(glyphs.front().cluster, 2u);
              EXPECT_THAT(glyphs.front().xPosition, DoubleNear(100, 1e-5));
              EXPECT_TRUE(std::isfinite(glyphs.front().yPosition));
            });
        driver.draw(document);
      });
}

TEST(TextIndentCompatibilityTest, LegacyRendererAggregateKeepsItsPreparedPayload) {
  auto prepared = std::make_shared<PreparedTextDraw>();
  const TextParams legacy{css::Color(css::RGBA()),
                          css::Color(css::RGBA()),
                          StrokeParams(),
                          {},
                          Lengthd(20),
                          Box2d(),
                          FontMetrics(),
                          TextAnchor::Start,
                          TextDecoration::None,
                          WritingMode::HorizontalTb,
                          0.0,
                          0.0,
                          std::nullopt,
                          LengthAdjust::Default,
                          100.0,
                          {},
                          entt::null,
                          entt::null,
                          prepared};
  EXPECT_EQ(legacy.preparedTextDraw, prepared);
  EXPECT_THAT(legacy.inlineSizePx, DoubleNear(100, 1e-5));
  EXPECT_THAT(legacy.textIndentPx, DoubleNear(0, 1e-5));
}

using DefaultIndentCase = std::tuple<bool, const char*>;
class TextIndentDefaultStateDeathTest : public testing::TestWithParam<DefaultIndentCase> {};

TEST_P(TextIndentDefaultStateDeathTest, UnspecifiedAncestorStillComputesUsableInitialValue) {
  const auto [onRoot, keyword] = GetParam();
  const std::string style = std::string("text-indent:") + keyword + ";";
  std::string scene =
      IndentScene("<tspan>A</tspan>", "inline-size:100px;" + (onRoot ? std::string() : style));
  if (onRoot) {
    scene.replace(0, 4, "<svg style='" + style + "'");
  }
  EXPECT_EXIT(
      {
        const auto observed =
            ObserveIndent(scene, false, "NotoSans-Regular.ttf", [](SVGDocument& document) {
              const auto text = document.querySelector("#t");
              ASSERT_THAT(text, Optional(_));
              const auto& properties = text->getComputedStyle();
              EXPECT_NE(properties.textIndent.state, PropertyState::Inherit);
              EXPECT_THAT(properties.textIndent.get(),
                          Optional(LengthIs(testing::DoubleEq(0), Eq(Lengthd::Unit::Px))));
            });
        EXPECT_THAT(observed.glyphs, SizeIs(1));
        if (observed.glyphs.size() == 1) {
          EXPECT_THAT(observed.glyphs.front().xPosition, DoubleNear(100, 1e-5));
          EXPECT_THAT(observed.glyphs.front().xAdvance, DoubleNear(10, 1e-5));
        }
        std::_Exit(testing::Test::HasFailure() ? 1 : 0);
      },
      testing::ExitedWithCode(0), "");
}

INSTANTIATE_TEST_SUITE_P(Defaults, TextIndentDefaultStateDeathTest,
                         testing::Combine(testing::Values(false, true),
                                          testing::Values("inherit", "unset")));

TEST(TextIndentTest, LeadingCollapsedWhitespaceAndEmptySpanKeepFirstWordAtLogicalZero) {
  const auto observed =
      ObserveIndent(IndentScene(" &#32;<tspan/>ABCDE",
                                "overflow-wrap:anywhere;inline-size:35px;text-indent:10px;"),
                    false, "NotoSans-Regular.ttf", {}, nullptr, true);
  ASSERT_THAT(observed.glyphs, SizeIs(5));
  std::vector<uint32_t> clusters;
  for (const auto& glyph : observed.glyphs) {
    clusters.push_back(glyph.cluster);
  }
  EXPECT_THAT(clusters, ElementsAreArray(std::vector<uint32_t>{1, 0, 4, 3, 2}));
  EXPECT_THAT(observed.glyphs[0].xPosition, DoubleNear(110, 1e-5));
  EXPECT_THAT(observed.glyphs[1].yPosition, DoubleNear(100, 1e-5));
  EXPECT_THAT(observed.glyphs[2].xPosition, DoubleNear(100, 1e-5));
  EXPECT_THAT(observed.glyphs[2].yPosition, DoubleNear(120, 1e-5));
}

TEST(TextIndentTest, NonzeroSpanSourceOffsetStillUsesRebasedFirstWordCapacity) {
  WithNativeIndentDocument(IndentScene("AB", ""), [](SVGDocument& document, const css::FontFace&) {
    FontManager fonts(document.registry());
    TextEngine engine(fonts, document.registry(), MakeIndentBackend(true));
    components::ComputedTextComponent text;
    components::ComputedTextComponent::TextSpan span;
    span.text = RcString("__ABCDE");
    span.start = 2;
    span.end = 7;
    span.xList.push_back(Lengthd(100));
    span.yList.push_back(Lengthd(100));
    span.startsNewChunk = true;
    span.overflowWrap = OverflowWrap::Anywhere;
    text.spans.push_back(std::move(span));
    TextLayoutParams params;
    params.fontFamilies = {RcString("IndentFace")};
    params.fontSize = Lengthd(20, Lengthd::Unit::Px);
    params.viewBox = Box2d::FromXYWH(0, 0, 400, 400);
    params.fontMetrics = FontMetrics::DefaultsWithFontSize(20);
    params.inlineSizePx = 35;
    params.textIndentPx = 10;
    const auto runs = engine.layout(text, params);
    ASSERT_THAT(runs, SizeIs(1));
    ASSERT_THAT(runs.front().glyphs, SizeIs(5));
    std::vector<uint32_t> clusters;
    for (const auto& glyph : runs.front().glyphs) {
      clusters.push_back(glyph.cluster);
    }
    EXPECT_THAT(clusters, ElementsAreArray(std::vector<uint32_t>{1, 0, 4, 3, 2}));
    EXPECT_EQ(runs.front().glyphs.front().glyphIndex, 'B' + 1);
    EXPECT_THAT(runs.front().glyphs[0].xPosition, DoubleNear(110, 1e-5));
    EXPECT_THAT(runs.front().glyphs[2].xPosition, DoubleNear(100, 1e-5));
    EXPECT_THAT(runs.front().glyphs[2].yPosition, DoubleNear(120, 1e-5));
  });
}

using SignedTabCase = std::tuple<double, double, double>;
using IndentTabCase = std::tuple<WritingMode, SignedTabCase>;
class TextIndentTabTest : public testing::TestWithParam<IndentTabCase> {};

TEST_P(TextIndentTabTest, PreservedTabUsesNextSignedCssStop) {
  const auto [mode, tabCase] = GetParam();
  const auto [indent, advance, after] = tabCase;
  const auto control = ObserveIndent(IndentScene(
      "&#9;B", WritingStyle(mode) + "white-space:pre;inline-size:200px;text-indent:0;"));
  const auto observed = ObserveIndent(
      IndentScene("&#9;B", WritingStyle(mode) + "white-space:pre;inline-size:200px;text-indent:" +
                               std::to_string(indent) + "px;"));
  ASSERT_THAT(control.glyphs, SizeIs(2));
  ASSERT_THAT(observed.glyphs, SizeIs(2));
  EXPECT_THAT(observed.codepoints, ElementsAreArray(std::vector<uint32_t>{'\t', 'B'}));
  EXPECT_THAT(InlinePen(control.glyphs[0], mode), DoubleNear(100, 1e-5));
  EXPECT_THAT(InlinePen(control.glyphs[1], mode), DoubleNear(180, 1e-5));
  EXPECT_THAT(
      mode == WritingMode::HorizontalTb ? control.glyphs[0].xAdvance : control.glyphs[0].yAdvance,
      DoubleNear(80, 1e-5));
  EXPECT_THAT(InlinePen(observed.glyphs[0], mode), DoubleNear(100 + indent, 1e-5));
  EXPECT_THAT(
      mode == WritingMode::HorizontalTb ? observed.glyphs[0].xAdvance : observed.glyphs[0].yAdvance,
      DoubleNear(advance, 1e-5));
  EXPECT_THAT(InlinePen(observed.glyphs[1], mode), DoubleNear(after, 1e-5));
  EXPECT_THAT(
      mode == WritingMode::HorizontalTb ? observed.glyphs[1].xAdvance : observed.glyphs[1].yAdvance,
      DoubleNear(10, 1e-5));
  EXPECT_THAT(BlockPen(observed.glyphs[0], mode),
              DoubleNear(BlockPen(control.glyphs[0], mode), 1e-5));
  EXPECT_THAT(BlockPen(observed.glyphs[1], mode),
              DoubleNear(BlockPen(observed.glyphs[0], mode), 1e-5));
}

INSTANTIATE_TEST_SUITE_P(
    AllAxes, TextIndentTabTest,
    testing::Combine(testing::Values(WritingMode::HorizontalTb, WritingMode::VerticalRl,
                                     WritingMode::VerticalLr),
                     testing::Values(SignedTabCase{-10, 10, 100}, SignedTabCase{-1, 81, 180},
                                     SignedTabCase{0, 80, 180}, SignedTabCase{10, 70, 180})));

}  // namespace
}  // namespace donner::svg
