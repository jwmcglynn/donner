#include "donner/editor/gui/EditorWindow.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#if defined(DONNER_EDITOR_WGPU)
#include "backends/imgui_impl_wgpu.h"
#include "donner/base/Box.h"
#include "donner/css/Color.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/StrokeParams.h"
#endif

namespace donner::editor::gui {
namespace {

#if defined(DONNER_EDITOR_WGPU)
std::array<std::uint8_t, 4> PixelAt(const svg::RendererBitmap& bitmap, int x, int y) {
  const std::size_t offset =
      static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
  return {bitmap.pixels[offset], bitmap.pixels[offset + 1], bitmap.pixels[offset + 2],
          bitmap.pixels[offset + 3]};
}
#endif

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

#if defined(DONNER_EDITOR_WGPU)
TEST(EditorWindowTest, WgpuPresentsGeodePremultipliedTextureWithoutDarkening) {
  EditorWindow window(EditorWindowOptions{
      .title = "Premultiplied WGPU Presentation Test",
      .initialWidth = 96,
      .initialHeight = 96,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  if (!window.valid() || window.geodeDevice() == nullptr) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }

  svg::RendererGeode source(window.geodeDevice());
  svg::RenderViewport viewport;
  viewport.size = Vector2d(64.0, 64.0);
  viewport.devicePixelRatio = 1.0;
  source.beginFrame(viewport);

  svg::PaintParams paint;
  paint.fill = svg::PaintServer::Solid{css::Color(css::RGBA(255, 0, 0, 128))};
  paint.opacity = 1.0;
  paint.fillOpacity = 1.0;
  source.setPaint(paint);
  source.drawRect(Box2d({0.0, 0.0}, {64.0, 64.0}), svg::StrokeParams{});
  source.endFrame();

  std::shared_ptr<const svg::RendererTextureSnapshot> texture = source.takeTextureSnapshot();
  ASSERT_TRUE(texture != nullptr);
  ASSERT_EQ(texture->backend(), svg::RendererTextureSnapshotBackend::Geode);
  const auto* geodeTexture = static_cast<const svg::RendererGeodeTextureSnapshot*>(texture.get());
  const WGPUTextureView textureView = geodeTexture->textureView();
  const ImTextureID textureId =
      static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(textureView));
  ImGui_ImplWGPU_SetTexturePremultipliedAlpha(textureId, true);

  window.beginFrame();
  ImGui::GetBackgroundDrawList()->AddImage(textureId, ImVec2(16.0f, 16.0f), ImVec2(80.0f, 80.0f));
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();

  ASSERT_FALSE(actual.empty());
  const std::array<std::uint8_t, 4> center = PixelAt(actual, 48, 48);
  EXPECT_NEAR(static_cast<int>(center[0]), 128, 3)
      << "A premultiplied red texture should not be multiplied by alpha again during ImGui "
         "presentation.";
  EXPECT_LE(center[1], 3);
  EXPECT_LE(center[2], 3);
  EXPECT_EQ(center[3], 255);
}
#endif

}  // namespace
}  // namespace donner::editor::gui
