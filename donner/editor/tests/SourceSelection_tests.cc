#include "donner/editor/SourceSelection.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "donner/base/xml/XMLNode.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/TextEditor.h"

namespace donner::editor {
namespace {

constexpr std::string_view kSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <g id="layer">
    <rect id="target" x="10" y="20" width="30" height="40"/>
  </g>
</svg>)";

TEST(SourceSelectionTest, HighlightsElementRangeResolvedFromSourceStoreOffset) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));

  auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  // Mutating through the source store advances the source version, so
  // node locations resolve through anchors as byte offsets without the
  // original parser line/column metadata.
  auto result = app.document().document().setElementAttribute(*target, "fill", "red");
  ASSERT_FALSE(result.diagnostic.has_value());

  auto xmlNode = xml::XMLNode::TryCast(target->entityHandle());
  ASSERT_TRUE(xmlNode.has_value());
  auto range = xmlNode->getNodeLocation();
  ASSERT_TRUE(range.has_value());
  ASSERT_TRUE(range->start.offset.has_value());
  ASSERT_TRUE(range->end.offset.has_value());
  EXPECT_FALSE(range->start.lineInfo.has_value());

  const std::string source(app.document().document().source());
  const std::string expectedSelectedSource =
      source.substr(*range->start.offset, *range->end.offset - *range->start.offset);

  TextEditor textEditor;
  textEditor.setText(source);
  ASSERT_TRUE(HighlightElementSource(textEditor, *target));

  EXPECT_EQ(textEditor.getSelectedText(), expectedSelectedSource);
  EXPECT_NE(textEditor.getSelectedText().find("<rect"), std::string::npos);
  EXPECT_NE(textEditor.getSelectedText().find("fill=\"red\""), std::string::npos);
}

TEST(SourceSelectionTest, HighlightsEachDonnerSplashLetterNode) {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }

  std::ostringstream splashBuffer;
  splashBuffer << splashStream.rdbuf();
  const std::string source = splashBuffer.str();

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(source));
  ASSERT_EQ(app.document().document().sourceVersion(), 0u);
  auto donner = app.document().document().querySelector("#Donner");
  ASSERT_TRUE(donner.has_value());

  TextEditor textEditor;
  textEditor.setText(source);

  int selectedLetterCount = 0;
  for (auto child = donner->firstChild(); child.has_value(); child = child->nextSibling()) {
    auto xmlNode = xml::XMLNode::TryCast(child->entityHandle());
    ASSERT_TRUE(xmlNode.has_value());
    auto range = xmlNode->getNodeLocation();
    ASSERT_TRUE(range.has_value());
    ASSERT_TRUE(range->start.offset.has_value());
    ASSERT_TRUE(range->end.offset.has_value());
    const std::string expectedSelectedSource =
        source.substr(*range->start.offset, *range->end.offset - *range->start.offset);

    ASSERT_TRUE(HighlightElementSource(textEditor, *child));
    const std::string selected = textEditor.getSelectedText();

    EXPECT_EQ(selected, expectedSelectedSource);
    EXPECT_TRUE(selected.starts_with("<path") || selected.starts_with("<polygon"))
        << "Selected source spilled before the letter node: " << selected.substr(0, 80);
    EXPECT_TRUE(selected.ends_with("/>")) << "Selected source spilled after the letter node: "
                                          << selected.substr(selected.size() - 80);
    EXPECT_EQ(selected.find("</g>"), std::string::npos) << selected;

    ++selectedLetterCount;
  }

  EXPECT_EQ(selectedLetterCount, 6);
}

}  // namespace
}  // namespace donner::editor
