#include "donner/editor/gui/EditorWindow.h"

#include <gtest/gtest.h>

namespace donner::editor::gui {
namespace {

TEST(EditorWindowTest, ComputeUiScaleConfigPrefersFramebufferRatio) {
  const UiScaleConfig config = ComputeUiScaleConfig(
      /*logicalWindowWidth=*/800, /*framebufferWidth=*/1600, /*contentScaleX=*/1.0);

  EXPECT_DOUBLE_EQ(config.displayScale, 2.0);
  EXPECT_FLOAT_EQ(config.scaledPixels(15.0), 30.0f);
  EXPECT_FLOAT_EQ(config.fontGlobalScale(), 0.5f);
}

TEST(EditorWindowTest, ComputeUiScaleConfigFallsBackToContentScale) {
  const UiScaleConfig config = ComputeUiScaleConfig(
      /*logicalWindowWidth=*/0, /*framebufferWidth=*/0, /*contentScaleX=*/1.5);

  EXPECT_DOUBLE_EQ(config.displayScale, 1.5);
  EXPECT_FLOAT_EQ(config.scaledPixels(14.0), 21.0f);
  EXPECT_NEAR(config.fontGlobalScale(), 1.0f / 1.5f, 1e-6f);
}

TEST(EditorWindowTest, ComputeUiScaleConfigClampsToOne) {
  const UiScaleConfig config = ComputeUiScaleConfig(
      /*logicalWindowWidth=*/800, /*framebufferWidth=*/400, /*contentScaleX=*/0.5);

  EXPECT_DOUBLE_EQ(config.displayScale, 1.0);
  EXPECT_FLOAT_EQ(config.scaledPixels(15.0), 15.0f);
  EXPECT_FLOAT_EQ(config.fontGlobalScale(), 1.0f);
}

}  // namespace
}  // namespace donner::editor::gui
