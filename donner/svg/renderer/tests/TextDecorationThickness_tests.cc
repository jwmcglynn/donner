#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/renderer/PlacedTextGeometry.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/tests/ParserTestUtils.h"
#include "donner/svg/text/TextBackend.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg {
namespace {

class DecorationBackend : public TextBackend {
public:
  explicit DecorationBackend(bool underlineMetrics = true)
      : hasUnderlineMetrics_(underlineMetrics) {}
  FontVMetrics fontVMetrics(FontHandle) const override {
    return {.ascent = 800, .descent = -200, .lineGap = 0, .xHeight = 500, .unitsPerEm = 1000};
  }
  float scaleForPixelHeight(FontHandle, float size) const override { return size / 1000; }
  float scaleForEmToPixels(FontHandle, float size) const override { return size / 1000; }
  std::optional<UnderlineMetrics> underlineMetrics(FontHandle) const override {
    return hasUnderlineMetrics_ ? std::optional(UnderlineMetrics{.position = -100, .thickness = 50})
                                : std::nullopt;
  }
  std::optional<UnderlineMetrics> strikeoutMetrics(FontHandle) const override {
    return UnderlineMetrics{.position = 300, .thickness = 40};
  }
  std::optional<SubSuperMetrics> subSuperMetrics(FontHandle) const override { return std::nullopt; }
  Path glyphOutline(FontHandle, int, float) const override {
    return PathBuilder().addRect(Box2d::FromXYWH(0, -2, 1, 2)).build();
  }
  bool isBitmapOnly(FontHandle) const override { return false; }
  bool isCursive(uint32_t) const override { return false; }
  bool hasSmallCapsFeature(FontHandle) const override { return false; }
  std::optional<BitmapGlyph> bitmapGlyph(FontHandle, int, float) const override {
    return std::nullopt;
  }
  ShapedRun shapeRun(FontHandle, float, std::string_view text, size_t begin, size_t length, bool,
                     FontVariant, bool) const override {
    ShapedRun run;
    for (size_t index = begin; index < std::min(text.size(), begin + length); ++index) {
      run.glyphs.push_back({.glyphIndex = 3,
                            .xAdvance = 10,
                            .yAdvance = 10,
                            .cluster = static_cast<uint32_t>(index)});
    }
    return run;
  }
  double crossSpanKern(FontHandle, float, FontHandle, float, uint32_t, uint32_t,
                       bool) const override {
    return 0;
  }

private:
  bool hasUnderlineMetrics_;
};

SVGDocument DecorationDocument(std::string_view body, bool metrics = true) {
  SVGDocument document = instantiateSubtree(body, {}, Vector2i(100, 100));
  auto& registry = document.registry();
  auto& fonts = registry.ctx().emplace<FontManager>(registry);
  registry.ctx().emplace<TextEngine>(fonts, registry, std::make_unique<DecorationBackend>(metrics));
  return document;
}

RendererBitmap RenderDecorationScene(std::string_view body, bool metrics = true) {
  auto document = DecorationDocument(body, metrics);
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

constexpr std::string_view kRedGlyphs =
    "<rect x='10' y='38' width='1' height='2' fill='red'/>"
    "<rect x='20' y='38' width='1' height='2' fill='red'/>";
constexpr std::string_view kRotatedGlyphs =
    "<rect x='5' y='80' width='1' height='2' fill='red'/>"
    "<rect x='10' y='80' width='1' height='2' fill='red'/>";

TEST(TextDecorationThickness, AutomaticUnderlineControl) {
  const auto actual = RenderDecorationScene(
      "<text x='10' y='40' font-size='20' fill='red' text-decoration='underline'>XX</text>");
  const auto expected = RenderDecorationScene(
      std::string(kRedGlyphs) + "<rect x='10' y='41.5' width='20' height='1' fill='red'/>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "decoration_auto_control",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(TextDecorationThickness, ExplicitThicknessAppliesToEachLine) {
  for (const auto [line, top] :
       {std::pair{"underline", 38}, std::pair{"overline", 20}, std::pair{"line-through", 30}}) {
    SCOPED_TRACE(line);
    const auto actual = RenderDecorationScene(
        "<text x='10' y='40' font-size='20' fill='red' style='text-decoration:" +
        std::string(line) + ";text-decoration-thickness:8px'>XX</text>");
    const auto expected =
        RenderDecorationScene(std::string(kRedGlyphs) + "<rect x='10' y='" + std::to_string(top) +
                              "' width='20' height='8' fill='red'/>");
    editor::tests::CompareBitmapToBitmap(actual, expected, line,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(TextDecorationThickness, ParentAndChildLinesKeepTheirOrigin) {
  const auto actual = RenderDecorationScene(R"svg(
    <text x="10" y="40" font-size="20" fill="red"
          style="text-decoration:underline;text-decoration-thickness:8px"><tspan font-size="40" fill="blue"
             style="text-decoration:line-through;text-decoration-thickness:2px">XX</tspan></text>)svg");
  const auto expected = RenderDecorationScene(std::string(kRedGlyphs) + R"svg(
    <rect x="10" y="38" width="20" height="8" fill="red"/><rect x="10" y="27" width="20" height="2" fill="blue"/>)svg");
  editor::tests::CompareBitmapToBitmap(actual, expected, "decoration_independent_origins",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(TextDecorationThickness, RepeatedSameLineKeepsBothOrigins) {
  const auto actual = RenderDecorationScene(R"svg(
    <text x="10" y="40" font-size="20" fill="red"
          style="text-decoration:underline;text-decoration-thickness:8px"><tspan fill="blue" style="text-decoration:underline;text-decoration-thickness:2px">XX</tspan></text>)svg");
  const auto expected = RenderDecorationScene(std::string(kRedGlyphs) + R"svg(
    <rect x="10" y="38" width="20" height="8" fill="red"/><rect x="10" y="41" width="20" height="2" fill="blue"/>)svg");
  editor::tests::CompareBitmapToBitmap(actual, expected, "decoration_repeated_origins",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(TextDecorationThickness, ChildThicknessWithoutLineDoesNotChangeParent) {
  const auto actual = RenderDecorationScene(R"svg(
    <text x="10" y="40" font-size="20" fill="red"
          style="text-decoration:underline;text-decoration-thickness:8px"><tspan font-size="40" fill="blue" style="text-decoration-thickness:2px">XX</tspan></text>)svg");
  const auto expected = RenderDecorationScene(
      std::string(kRedGlyphs) + "<rect x='10' y='38' width='20' height='8' fill='red'/>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "decoration_unrelated_child_thickness",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(TextDecorationThickness, PercentageUsesOriginUsedFontSize) {
  const auto actual = RenderDecorationScene(
      R"svg(<text x="10" y="40" font-size="20" fill="red" style="text-decoration:underline;text-decoration-thickness:10%"><tspan font-size="40">XX</tspan></text>)svg");
  const auto expected = RenderDecorationScene(
      std::string(kRedGlyphs) + "<rect x='10' y='41' width='20' height='2' fill='red'/>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "decoration_percentage_origin",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(TextDecorationThickness, FromFontUsesUnderlineMetricForLineThrough) {
  const auto actual = RenderDecorationScene(
      R"svg(<text x="10" y="40" font-size="20" fill="red" style="text-decoration:line-through;text-decoration-thickness:from-font">XX</text>)svg");
  const auto expected = RenderDecorationScene(
      std::string(kRedGlyphs) + "<rect x='10' y='33.5' width='20' height='1' fill='red'/>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "decoration_from_font",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(TextDecorationThickness, MissingUnderlineMetricFallsBackToAuto) {
  const auto actual = RenderDecorationScene(
      R"svg(<text x="10" y="40" font-size="20" fill="red" style="text-decoration:underline;text-decoration-thickness:from-font">XX</text>)svg",
      false);
  const auto expected = RenderDecorationScene(
      R"svg(<text x="10" y="40" font-size="20" fill="red" text-decoration="underline">XX</text>)svg",
      false);
  editor::tests::CompareBitmapToBitmap(actual, expected, "decoration_missing_metric",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(TextDecorationThickness, PercentageMatchesLengthInEveryWritingMode) {
  for (const auto mode : {"horizontal-tb", "vertical-rl", "vertical-lr"}) {
    SCOPED_TRACE(mode);
    const std::string prefix =
        "<text x='50' y='40' font-size='20' fill='red' style='writing-mode:" + std::string(mode) +
        ";text-decoration:underline overline line-through;";
    const auto actual = RenderDecorationScene(prefix + "text-decoration-thickness:20%'>XX</text>");
    const auto expected =
        RenderDecorationScene(prefix + "text-decoration-thickness:4px'>XX</text>");
    editor::tests::CompareBitmapToBitmap(actual, expected, "decoration_writing_mode",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(TextDecorationThickness, StyleMutationUpdatesThickness) {
  auto document = DecorationDocument(
      "<text id='t' x='10' y='40' font-size='20' fill='red' "
      "style='text-decoration:underline;text-decoration-thickness:8px'>XX</text>");
  auto text = document.querySelector("#t");
  ASSERT_TRUE(text.has_value());
  for (const auto [thickness, top] : {std::pair{8, 38}, std::pair{2, 41}}) {
    text->setAttribute("style", "text-decoration:underline;text-decoration-thickness:" +
                                    std::to_string(thickness) + "px");
    const auto actual = RenderDocumentWithBackend(document, ActiveRendererBackend());
    const auto expected = RenderDecorationScene(std::string(kRedGlyphs) + "<rect x='10' y='" +
                                                std::to_string(top) + "' width='20' height='" +
                                                std::to_string(thickness) + "' fill='red'/>");
    editor::tests::CompareBitmapToBitmap(actual, expected, "decoration_style_mutation",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(TextDecorationThickness, RotatedMinimumUsesTransformedSegmentNormal) {
  for (const auto value : {"0px", "-1px", "-5%"}) {
    SCOPED_TRACE(value);
    const auto actual = RenderDecorationScene(
        "<g transform='scale(.5 2)'><text x='10' y='40' "
        "rotate='90' font-size='20' fill='red' style='text-decoration:underline;"
        "text-decoration-thickness:" +
        std::string(value) + "'>XX</text></g>");
    const auto expected =
        RenderDecorationScene(std::string(kRotatedGlyphs) +
                              "<rect x='3.5' y='80' width='1' height='20' fill='red'/>"
                              "<rect x='8.5' y='80' width='1' height='20' fill='red'/>");
    editor::tests::CompareBitmapToBitmap(actual, expected, "decoration_rotated_minimum",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(TextDecorationThickness, OriginatingFillNoneDoesNotUseDescendantPaint) {
  const auto actual = RenderDecorationScene(R"svg(
    <text x="10" y="40" font-size="20" fill="none"
          style="text-decoration:underline;text-decoration-thickness:8px"><tspan fill="red"
          style="text-decoration:underline;text-decoration-thickness:2px">XX</tspan></text>)svg");
  const auto expected = RenderDecorationScene(
      std::string(kRedGlyphs) + "<rect x='10' y='41' width='20' height='2' fill='red'/>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "decoration_origin_fill_none",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(TextDecorationThickness, OriginPatternRemainsAvailableWithDescendantSolidPaint) {
  constexpr std::string_view pattern =
      "<pattern id='p' patternUnits='userSpaceOnUse' width='4' height='4'>"
      "<rect width='2' height='4' fill='red'/>"
      "<rect x='2' width='2' height='4' fill='blue'/></pattern>";
  for (const bool child : {false, true}) {
    SCOPED_TRACE(child);
    const std::string text = child ? "<tspan fill='lime' style='text-decoration:line-through;"
                                     "text-decoration-thickness:2px'>XX</tspan>"
                                   : "XX";
    const auto actual =
        RenderDecorationScene(std::string(pattern) +
                              "<text x='10' y='40' font-size='20' fill='url(#p)' "
                              "style='text-decoration:underline;text-decoration-thickness:8px'>" +
                              text + "</text>");
    const std::string glyphFill = child ? "lime" : "url(#p)";
    const auto expected = RenderDecorationScene(
        std::string(pattern) + "<rect x='10' y='38' width='1' height='2' fill='" + glyphFill +
        "'/><rect x='20' y='38' width='1' height='2' fill='" + glyphFill +
        "'/><rect x='10' y='38' width='20' height='8' fill='url(#p)'/>" +
        (child ? "<rect x='10' y='33' width='20' height='2' fill='lime'/>" : ""));
    editor::tests::CompareBitmapToBitmap(actual, expected, "decoration_origin_pattern",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(TextDecorationThickness, ShearedMinimumUsesPerpendicularDeviceWidth) {
  for (const auto value : {"0px", "-1px", "0.1px"}) {
    SCOPED_TRACE(value);
    const auto actual = RenderDecorationScene(
        "<g transform='matrix(1 0 10 1 0 0)'><text x='4' y='4' font-size='20' fill='red' "
        "style='text-decoration:underline;text-decoration-thickness:" +
        std::string(value) + "'>XX</text></g>");
    const auto expected = RenderDecorationScene(
        "<g transform='matrix(1 0 10 1 0 0)'>"
        "<rect x='4' y='2' width='1' height='2' fill='red'/>"
        "<rect x='14' y='2' width='1' height='2' fill='red'/>"
        "<rect x='4' y='5.5' width='20' height='1' fill='red'/></g>");
    editor::tests::CompareBitmapToBitmap(actual, expected, "decoration_sheared_minimum",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(TextDecorationThickness, SingularAndNonFiniteTransformsSuppressExplicitBands) {
  auto document = DecorationDocument("");
  const auto& engine = document.registry().ctx().get<TextEngine>();
  TextRun run;
  TextGlyph glyph;
  glyph.glyphIndex = 3;
  glyph.xAdvance = 10;
  run.glyphs.push_back(glyph);
  components::TextDecorationOrigin origin;
  origin.lines = TextDecoration::Underline;
  origin.thickness = {TextDecorationThickness::Kind::Length, Lengthd(0)};
  const double infinity = std::numeric_limits<double>::infinity();
  const double maximum = std::numeric_limits<double>::max();
  for (const auto& linear : {std::array{1.0, 0.0, 10.0, 0.0}, std::array{1.0, 1.0, 10.0, 10.0},
                             std::array{0.0, 0.0, 1.0, 1.0}, std::array{1.0, 0.0, 0.0, infinity},
                             std::array{maximum, maximum, 0.0, 1.0}}) {
    Transform2d transform;
    std::copy(linear.begin(), linear.end(), transform.data);
    std::size_t remaining = Path::kMaximumGeometryQueryWork;
    bool emitted = false;
    EmitTextDecorationPaths(run, origin, 1, 20, engine, transform, remaining,
                            [&](const Path&) { emitted = true; });
    EXPECT_FALSE(emitted);
  }
}

TEST(TextDecorationThickness, ImmutableOriginStorageRefusesAndReleasesCapacity) {
  using components::DocumentResourceFamilyBudget;
  using components::TextDecorationData;
  using components::TextDecorationOrigin;
  DocumentResourceFamilyBudget::Limits limits;
  limits.geometryBytes = sizeof(TextDecorationData) + sizeof(TextDecorationOrigin);
  auto family = std::make_shared<DocumentResourceFamilyBudget>(limits);
  const std::array<TextDecorationOrigin, 1> origins{};
  auto data = TextDecorationData::Create(family, origins);
  ASSERT_TRUE(data);
  EXPECT_EQ(family->retainedBytes(DocumentResourceFamilyBudget::Kind::Geometry),
            limits.geometryBytes);
  const std::array<TextDecorationOrigin, TextDecorationData::kMaximumOrigins + 1> tooMany{};
  EXPECT_FALSE(TextDecorationData::Create(family, tooMany));
  auto shared = data;
  EXPECT_FALSE(TextDecorationData::Create(family, origins));
  data.reset();
  EXPECT_EQ(family->retainedBytes(DocumentResourceFamilyBudget::Kind::Geometry),
            limits.geometryBytes);
  shared.reset();
  EXPECT_EQ(family->retainedBytes(DocumentResourceFamilyBudget::Kind::Geometry), 0u);
}

}  // namespace
}  // namespace donner::svg
