#include "donner/editor/EditorSampleCatalog.h"

#include <array>
#include <cctype>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "donner/base/Box.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

TEST(EditorSampleCatalog, HasStableUniqueAsciiIdsInDisplayOrder) {
  const std::span<const EditorSample> samples = GetEditorSampleCatalog();
  ASSERT_EQ(samples.size(), 4u);

  constexpr std::array<std::string_view, 4> kExpectedIds = {"donner-splash", "basic-shapes",
                                                            "text-style", "gradients-clip"};
  std::unordered_set<std::string_view> ids;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    EXPECT_EQ(samples[i].id, kExpectedIds[i]);
    EXPECT_TRUE(ids.insert(samples[i].id).second);
    for (const unsigned char character : samples[i].id) {
      EXPECT_LT(character, 0x80u);
      EXPECT_TRUE(std::isalnum(character) || character == '-')
          << "non-stable ID character in " << samples[i].id;
    }
  }
}

TEST(EditorSampleCatalog, EntriesHaveTitlesAndSources) {
  for (const EditorSample& sample : GetEditorSampleCatalog()) {
    EXPECT_FALSE(sample.id.empty());
    EXPECT_FALSE(sample.title.empty()) << sample.id;
    EXPECT_FALSE(sample.source.empty()) << sample.id;
    EXPECT_EQ(FindEditorSample(sample.id), &sample);
  }
  EXPECT_EQ(FindEditorSample("missing-sample"), nullptr);
}

TEST(EditorSampleCatalog, SourcesParseWithUsableRootDimensions) {
  for (const EditorSample& sample : GetEditorSampleCatalog()) {
    ParseWarningSink warningSink = ParseWarningSink::Disabled();
    auto result = svg::parser::SVGParser::ParseSVG(sample.source, warningSink);
    ASSERT_FALSE(result.hasError()) << sample.id << ": " << result.error();

    svg::SVGDocument document = std::move(result).result();
    const svg::SVGSVGElement root = document.svgElement();
    const std::optional<Box2d> viewBox = root.viewBox();
    ASSERT_TRUE(viewBox.has_value()) << sample.id << " has no viewBox";
    EXPECT_GT(viewBox->width(), 0.0) << sample.id;
    EXPECT_GT(viewBox->height(), 0.0) << sample.id;

    ASSERT_TRUE(root.width().has_value()) << sample.id << " has no width";
    ASSERT_TRUE(root.height().has_value()) << sample.id << " has no height";
    EXPECT_GT(root.width()->value, 0.0) << sample.id;
    EXPECT_GT(root.height()->value, 0.0) << sample.id;
  }
}

TEST(EditorSampleCatalog, SourcesRenderNonemptyPickerThumbnails) {
  svg::Renderer renderer;
  for (const EditorSample& sample : GetEditorSampleCatalog()) {
    ParseWarningSink warningSink = ParseWarningSink::Disabled();
    auto result = svg::parser::SVGParser::ParseSVG(sample.source, warningSink);
    ASSERT_FALSE(result.hasError()) << sample.id << ": " << result.error();

    svg::SVGDocument document = std::move(result).result();
    document.setCanvasSize(192, 120);
    renderer.draw(document);
    const svg::RendererBitmap bitmap = renderer.takeSnapshot();
    EXPECT_FALSE(bitmap.empty()) << sample.id;
    EXPECT_GT(bitmap.dimensions.x, 0) << sample.id;
    EXPECT_GT(bitmap.dimensions.y, 0) << sample.id;
    EXPECT_LE(bitmap.dimensions.x, 192) << sample.id;
    EXPECT_LE(bitmap.dimensions.y, 120) << sample.id;
  }
}

}  // namespace
}  // namespace donner::editor
