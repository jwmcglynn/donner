#include "donner/editor/SidebarPresenter.h"

#include <span>
#include <string_view>

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

constexpr std::string_view kInspectorSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
         <rect x="10" y="20" width="100" height="50" fill="red" id="target"/>
         <rect id="peer" x="0" y="0" width="10" height="10"/>
       </svg>)";

const std::string* FindInspectorValue(std::span<const std::pair<std::string, std::string>> entries,
                                      std::string_view name) {
  for (const auto& [entryName, entryValue] : entries) {
    if (entryName == name) {
      return &entryValue;
    }
  }

  return nullptr;
}

TEST(SidebarPresenterTest, RefreshSnapshotCapturesXmlAttributesAndComputedStyle) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);

  auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  EXPECT_TRUE(presenter.inspectorHasSelectionForTesting());

  const auto xmlAttributes = presenter.inspectorXmlAttributesForTesting();
  ASSERT_EQ(xmlAttributes.size(), 6u);
  EXPECT_EQ(xmlAttributes[0].first, "x");
  EXPECT_EQ(xmlAttributes[0].second, "10");
  EXPECT_EQ(xmlAttributes[1].first, "y");
  EXPECT_EQ(xmlAttributes[1].second, "20");
  EXPECT_EQ(xmlAttributes[2].first, "width");
  EXPECT_EQ(xmlAttributes[2].second, "100");
  EXPECT_EQ(xmlAttributes[3].first, "height");
  EXPECT_EQ(xmlAttributes[3].second, "50");
  EXPECT_EQ(xmlAttributes[4].first, "fill");
  EXPECT_EQ(xmlAttributes[4].second, "red");
  EXPECT_EQ(xmlAttributes[5].first, "id");
  EXPECT_EQ(xmlAttributes[5].second, "target");

  const auto computedStyle = presenter.inspectorComputedStyleForTesting();
  ASSERT_EQ(computedStyle.size(), 9u);
  EXPECT_EQ(computedStyle[0].first, "display");
  EXPECT_EQ(computedStyle[1].first, "visibility");
  EXPECT_EQ(computedStyle[2].first, "opacity");
  EXPECT_EQ(computedStyle[3].first, "fill");
  EXPECT_EQ(computedStyle[4].first, "fill-opacity");
  EXPECT_EQ(computedStyle[5].first, "stroke");
  EXPECT_EQ(computedStyle[6].first, "stroke-width");
  EXPECT_EQ(computedStyle[7].first, "stroke-opacity");
  EXPECT_EQ(computedStyle[8].first, "color");

  const std::string* displayValue = FindInspectorValue(computedStyle, "display");
  ASSERT_NE(displayValue, nullptr);
  EXPECT_EQ(*displayValue, "inline (default)");

  const std::string* fillValue = FindInspectorValue(computedStyle, "fill");
  ASSERT_NE(fillValue, nullptr);
  EXPECT_EQ(*fillValue, "PaintServer(solid rgba(255, 0, 0, 255)) (set)");

  const std::string* strokeValue = FindInspectorValue(computedStyle, "stroke");
  ASSERT_NE(strokeValue, nullptr);
  EXPECT_EQ(*strokeValue, "PaintServer(none) (default)");

  const std::string* colorValue = FindInspectorValue(computedStyle, "color");
  ASSERT_NE(colorValue, nullptr);
  EXPECT_EQ(*colorValue, "rgba(0, 0, 0, 255) (default)");
}

TEST(SidebarPresenterTest, RefreshSnapshotOmitsInspectorDetailsForMultiSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);

  auto target = app.document().document().querySelector("#target");
  auto peer = app.document().document().querySelector("#peer");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(peer.has_value());

  app.setSelection(std::vector<svg::SVGElement>{*target, *peer});

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  EXPECT_FALSE(presenter.inspectorHasSelectionForTesting());
  EXPECT_TRUE(presenter.inspectorXmlAttributesForTesting().empty());
  EXPECT_TRUE(presenter.inspectorComputedStyleForTesting().empty());
}

}  // namespace
}  // namespace donner::editor
