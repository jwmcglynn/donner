#include "donner/editor/gui/EditorWindow.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#if defined(DONNER_EDITOR_WGPU)
#include "backends/imgui_impl_wgpu.h"
#include "donner/base/Box.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/css/Color.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererImageIO.h"
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

double LumaAt(const svg::RendererBitmap& bitmap, int x, int y) {
  const std::array<std::uint8_t, 4> pixel = PixelAt(bitmap, x, y);
  return 0.299 * static_cast<double>(pixel[0]) + 0.587 * static_cast<double>(pixel[1]) +
         0.114 * static_cast<double>(pixel[2]);
}

std::array<std::uint8_t, 4> PixelAtLogical(const svg::RendererBitmap& bitmap,
                                           const Vector2d& readbackFromLogical, double logicalX,
                                           double logicalY) {
  return PixelAt(bitmap, static_cast<int>(std::lround(logicalX * readbackFromLogical.x)),
                 static_cast<int>(std::lround(logicalY * readbackFromLogical.y)));
}

double MeanLumaAt(const svg::RendererBitmap& bitmap, int centerX, int centerY, int radius) {
  double total = 0.0;
  int count = 0;
  for (int y = centerY - radius; y <= centerY + radius; ++y) {
    if (y < 0 || y >= bitmap.dimensions.y) {
      continue;
    }
    for (int x = centerX - radius; x <= centerX + radius; ++x) {
      if (x < 0 || x >= bitmap.dimensions.x) {
        continue;
      }
      total += LumaAt(bitmap, x, y);
      ++count;
    }
  }
  return count > 0 ? total / static_cast<double>(count) : 0.0;
}

Vector2d ReadbackScale(const svg::RendererBitmap& bitmap, int logicalWidth, int logicalHeight) {
  return Vector2d(static_cast<double>(bitmap.dimensions.x) / static_cast<double>(logicalWidth),
                  static_cast<double>(bitmap.dimensions.y) / static_cast<double>(logicalHeight));
}

double MeanLumaAtLogical(const svg::RendererBitmap& bitmap, const Vector2d& readbackFromLogical,
                         double logicalX, double logicalY, double logicalRadius) {
  return MeanLumaAt(bitmap, static_cast<int>(std::lround(logicalX * readbackFromLogical.x)),
                    static_cast<int>(std::lround(logicalY * readbackFromLogical.y)),
                    static_cast<int>(std::lround(logicalRadius * readbackFromLogical.x)));
}

void WriteDiagnosticBitmap(const svg::RendererBitmap& bitmap, std::string_view filename) {
  const char* outputDir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  if (outputDir == nullptr || bitmap.empty()) {
    return;
  }

  const std::filesystem::path outputPath = std::filesystem::path(outputDir) / filename;
  svg::RendererImageIO::writeRgbaPixelsToPngFile(outputPath.string().c_str(), bitmap.pixels,
                                                 bitmap.dimensions.x, bitmap.dimensions.y,
                                                 bitmap.rowBytes / 4u);
}

std::shared_ptr<const svg::RendererTextureSnapshot> RenderBlurredGlowTexture(
    const std::shared_ptr<geode::GeodeDevice>& device) {
  constexpr std::string_view kBlurredGlowSvg = R"SVG(
    <svg xmlns="http://www.w3.org/2000/svg" width="96" height="96" viewBox="0 0 96 96">
      <defs>
        <filter id="blur" x="-50%" y="-50%" width="200%" height="200%">
          <feGaussianBlur stdDeviation="7"/>
        </filter>
      </defs>
      <circle cx="48" cy="48" r="20" fill="#ff8c00" fill-opacity="0.58" filter="url(#blur)"/>
    </svg>
  )SVG";

  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  auto parsed = svg::parser::SVGParser::ParseSVG(kBlurredGlowSvg, warningSink);
  EXPECT_FALSE(parsed.hasError()) << parsed.error();
  if (parsed.hasError()) {
    return nullptr;
  }

  svg::SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(96, 96);

  svg::Renderer renderer(device);
  renderer.draw(document);
  return renderer.takeTextureSnapshot();
}

std::shared_ptr<const svg::RendererTextureSnapshot> RenderPremultipliedRedTexture(
    const std::shared_ptr<geode::GeodeDevice>& device) {
  svg::RendererGeode source(device);
  svg::RenderViewport viewport;
  viewport.size = Vector2d(32.0, 32.0);
  viewport.devicePixelRatio = 1.0;
  source.beginFrame(viewport);

  svg::PaintParams paint;
  paint.fill = svg::PaintServer::Solid{css::Color(css::RGBA(255, 0, 0, 128))};
  paint.opacity = 1.0;
  paint.fillOpacity = 1.0;
  source.setPaint(paint);
  source.drawRect(Box2d({0.0, 0.0}, {32.0, 32.0}), svg::StrokeParams{});
  source.endFrame();
  return source.takeTextureSnapshot();
}

std::optional<svg::SVGDocument> LoadDonnerSplashDocument() {
  std::ifstream splashStream("donner_splash.svg");
  if (!splashStream.is_open()) {
    return std::nullopt;
  }

  std::ostringstream splashBuf;
  splashBuf << splashStream.rdbuf();
  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  auto parsed = svg::parser::SVGParser::ParseSVG(splashBuf.str(), warningSink);
  EXPECT_FALSE(parsed.hasError()) << parsed.error();
  if (parsed.hasError()) {
    return std::nullopt;
  }

  svg::SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(892, 512);
  return document;
}

ImTextureID TextureIdFromGeodeSnapshot(const svg::RendererTextureSnapshot& texture) {
  if (texture.backend() != svg::RendererTextureSnapshotBackend::Geode) {
    return 0;
  }

  const auto& geodeTexture = static_cast<const svg::RendererGeodeTextureSnapshot&>(texture);
  const WGPUTextureView textureView = geodeTexture.textureView();
  return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(textureView));
}

std::optional<RenderResult::CompositedPreview> RenderCompositedPreview(
    const std::shared_ptr<geode::GeodeDevice>& device, svg::SVGDocument& document,
    Entity targetEntity) {
  svg::Renderer renderer(device);
  AsyncRenderer asyncRenderer;
  RenderRequest request(renderer, document);
  request.version = 1;
  request.selectedEntity = targetEntity;
  request.dragPreview = RenderRequest::DragPreview{
      .entity = targetEntity,
      .interactionKind = svg::compositor::InteractionHint::Selection,
  };
  asyncRenderer.requestRender(request);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  std::optional<RenderResult> result;
  while (std::chrono::steady_clock::now() < deadline) {
    result = asyncRenderer.pollResult();
    if (result.has_value()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_TRUE(result.has_value());
  if (!result.has_value() || !result->compositedPreview.has_value()) {
    return std::nullopt;
  }

  return std::move(result->compositedPreview);
}

std::optional<RenderResult::CompositedPreview> RenderBlurredGlowCompositedPreview(
    const std::shared_ptr<geode::GeodeDevice>& device) {
  constexpr std::string_view kCompositedGlowSvg = R"SVG(
    <svg xmlns="http://www.w3.org/2000/svg" width="160" height="160" viewBox="0 0 160 160">
      <defs>
        <filter id="blur" x="-50%" y="-50%" width="200%" height="200%">
          <feGaussianBlur in="SourceGraphic" stdDeviation="7"/>
        </filter>
      </defs>
      <rect width="160" height="160" fill="#0d0f1d"/>
      <g id="glow" filter="url(#blur)">
        <circle cx="80" cy="80" r="22" fill="#ff8c00" fill-opacity="0.58"/>
      </g>
    </svg>
  )SVG";

  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  auto parsed = svg::parser::SVGParser::ParseSVG(kCompositedGlowSvg, warningSink);
  EXPECT_FALSE(parsed.hasError()) << parsed.error();
  if (parsed.hasError()) {
    return std::nullopt;
  }

  svg::SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(160, 160);
  std::optional<svg::SVGElement> glow = document.querySelector("#glow");
  EXPECT_TRUE(glow.has_value());
  if (!glow.has_value()) {
    return std::nullopt;
  }

  return RenderCompositedPreview(device, document, glow->entityHandle().entity());
}

void DrawCompositedTiles(const GlTextureCache& textures, const ImVec2& origin, double zoom) {
  ImDrawList* drawList = ImGui::GetBackgroundDrawList();
  for (const GlTextureCache::TileView& tile : textures.tiles()) {
    if (tile.texture == 0) {
      continue;
    }

    const ImVec2 topLeft(
        origin.x + static_cast<float>((tile.canvasOffsetDoc.x + tile.dragTranslationDoc.x) * zoom),
        origin.y + static_cast<float>((tile.canvasOffsetDoc.y + tile.dragTranslationDoc.y) * zoom));
    const ImVec2 bottomRight(topLeft.x + static_cast<float>(tile.bitmapDimsDoc.x * zoom),
                             topLeft.y + static_cast<float>(tile.bitmapDimsDoc.y * zoom));
    drawList->AddImage(tile.texture, topLeft, bottomRight);
  }
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
  ImGui_ImplWGPU_AddTexturePremultipliedAlphaRef(textureId);

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
  ImGui_ImplWGPU_RemoveTexturePremultipliedAlphaRef(textureId);
  ImGui_ImplWGPU_RemoveTexture(textureId);
}

TEST(EditorWindowTest, WgpuPremultipliedTextureSurvivesOnePresentationOwnerRetiring) {
  EditorWindow window(EditorWindowOptions{
      .title = "Shared Premultiplied WGPU Texture Ownership Test",
      .initialWidth = 128,
      .initialHeight = 96,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  if (!window.valid() || window.geodeDevice() == nullptr) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }

  std::shared_ptr<const svg::RendererTextureSnapshot> texture =
      RenderPremultipliedRedTexture(window.geodeDevice());
  ASSERT_TRUE(texture != nullptr);

  GlTextureCache liveOwner;
  GlTextureCache transientOwner;
  liveOwner.initialize();
  transientOwner.initialize();
  liveOwner.uploadOverlayTexture(texture);
  transientOwner.uploadOverlayTexture(texture);
  ASSERT_NE(liveOwner.overlayTexture(), 0);

  window.beginFrame();
  ImGui::GetBackgroundDrawList()->AddImage(liveOwner.overlayTexture(), ImVec2(16.0f, 16.0f),
                                           ImVec2(48.0f, 48.0f));
  const svg::RendererBitmap expected = window.endFrameAndReadPixels();
  WriteDiagnosticBitmap(expected, "premul_shared_owner_before_retire_presentation.png");

  transientOwner.clearOverlay();
  for (int i = 0; i < 5; ++i) {
    transientOwner.advancePresentationFrame();
  }

  window.beginFrame();
  ImGui::GetBackgroundDrawList()->AddImage(liveOwner.overlayTexture(), ImVec2(16.0f, 16.0f),
                                           ImVec2(48.0f, 48.0f));
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();
  WriteDiagnosticBitmap(actual, "premul_shared_owner_retire_presentation.png");

  ASSERT_FALSE(actual.empty());
  const Vector2d readbackFromLogical = ReadbackScale(actual, 128, 96);
  const std::array<std::uint8_t, 4> center =
      PixelAtLogical(actual, readbackFromLogical, 32.0, 32.0);
  EXPECT_NEAR(static_cast<int>(center[0]), 128, 3)
      << "One presentation owner retiring a shared Geode texture must not clear the "
         "premultiplied-alpha registration while another owner still draws it.";
  EXPECT_LE(center[1], 3);
  EXPECT_LE(center[2], 3);
  EXPECT_EQ(center[3], 255);
}

TEST(EditorWindowTest, WgpuPresentsZoomedBlurredPremultipliedTextureWithoutDarkening) {
  EditorWindow window(EditorWindowOptions{
      .title = "Zoomed Premultiplied WGPU Presentation Test",
      .initialWidth = 368,
      .initialHeight = 232,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  if (!window.valid() || window.geodeDevice() == nullptr) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }

  std::shared_ptr<const svg::RendererTextureSnapshot> texture =
      RenderBlurredGlowTexture(window.geodeDevice());
  ASSERT_TRUE(texture != nullptr);
  const ImTextureID textureId = TextureIdFromGeodeSnapshot(*texture);
  ASSERT_NE(textureId, 0);
  ImGui_ImplWGPU_AddTexturePremultipliedAlphaRef(textureId);

  window.beginFrame();
  ImDrawList* drawList = ImGui::GetBackgroundDrawList();
  drawList->AddImage(textureId, ImVec2(16.0f, 68.0f), ImVec2(112.0f, 164.0f));
  drawList->AddImage(textureId, ImVec2(144.0f, 20.0f), ImVec2(336.0f, 212.0f));
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();
  WriteDiagnosticBitmap(actual, "zoomed_blurred_premul_presentation.png");

  ASSERT_FALSE(actual.empty());
  const Vector2d readbackFromLogical = ReadbackScale(actual, 368, 232);
  const double referenceLuma =
      MeanLumaAtLogical(actual, readbackFromLogical, 16.0 + 48.0, 68.0 + 48.0, 1.0);
  const double zoomedLuma =
      MeanLumaAtLogical(actual, readbackFromLogical, 144.0 + 96.0, 20.0 + 96.0, 2.0);
  EXPECT_NEAR(zoomedLuma, referenceLuma, 4.0)
      << "A zoomed premultiplied blurred texture should preserve the same center glow intensity "
         "as the 1x presentation. reference="
      << referenceLuma << " zoomed=" << zoomedLuma;

  const double referenceEdgeLuma =
      MeanLumaAtLogical(actual, readbackFromLogical, 16.0 + 74.0, 68.0 + 48.0, 1.0);
  const double zoomedEdgeLuma =
      MeanLumaAtLogical(actual, readbackFromLogical, 144.0 + 148.0, 20.0 + 96.0, 2.0);
  EXPECT_NEAR(zoomedEdgeLuma, referenceEdgeLuma, 4.0)
      << "A zoomed premultiplied blurred texture should preserve the same transparent-edge "
         "intensity as the 1x presentation. reference="
      << referenceEdgeLuma << " zoomed=" << zoomedEdgeLuma;
  ImGui_ImplWGPU_RemoveTexturePremultipliedAlphaRef(textureId);
  ImGui_ImplWGPU_RemoveTexture(textureId);
}

TEST(EditorWindowTest, WgpuPresentsZoomedCompositedBlurredLayerWithoutDarkening) {
  EditorWindow window(EditorWindowOptions{
      .title = "Zoomed Composited Premultiplied WGPU Presentation Test",
      .initialWidth = 592,
      .initialHeight = 368,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  if (!window.valid() || window.geodeDevice() == nullptr) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }

  std::optional<RenderResult::CompositedPreview> preview =
      RenderBlurredGlowCompositedPreview(window.geodeDevice());
  ASSERT_TRUE(preview.has_value());
  ASSERT_GE(preview->tiles.size(), 2u) << "Expected a background segment plus a filter layer";

  GlTextureCache textures;
  textures.initialize();
  textures.uploadComposited(*preview);
  ASSERT_GE(textures.tiles().size(), 2u);

  window.beginFrame();
  DrawCompositedTiles(textures, ImVec2(16.0f, 24.0f), 1.0);
  DrawCompositedTiles(textures, ImVec2(240.0f, 24.0f), 2.0);
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();
  WriteDiagnosticBitmap(actual, "zoomed_composited_blurred_layer_presentation.png");

  ASSERT_FALSE(actual.empty());
  const Vector2d readbackFromLogical = ReadbackScale(actual, 592, 368);
  const double referenceCenterLuma =
      MeanLumaAtLogical(actual, readbackFromLogical, 16.0 + 80.0, 24.0 + 80.0, 2.0);
  const double zoomedCenterLuma =
      MeanLumaAtLogical(actual, readbackFromLogical, 240.0 + 160.0, 24.0 + 160.0, 4.0);
  EXPECT_NEAR(zoomedCenterLuma, referenceCenterLuma, 5.0)
      << "A zoomed composited filter layer should preserve center glow intensity. reference="
      << referenceCenterLuma << " zoomed=" << zoomedCenterLuma;

  const double referenceEdgeLuma =
      MeanLumaAtLogical(actual, readbackFromLogical, 16.0 + 112.0, 24.0 + 80.0, 2.0);
  const double zoomedEdgeLuma =
      MeanLumaAtLogical(actual, readbackFromLogical, 240.0 + 224.0, 24.0 + 160.0, 4.0);
  EXPECT_NEAR(zoomedEdgeLuma, referenceEdgeLuma, 5.0)
      << "A zoomed composited filter layer should preserve transparent-edge intensity. reference="
      << referenceEdgeLuma << " zoomed=" << zoomedEdgeLuma;
}

TEST(EditorWindowTest, WgpuPresentsZoomedDonnerSplashFilteredLayerWithoutDarkening) {
  EditorWindow window(EditorWindowOptions{
      .title = "Zoomed Donner Splash Premultiplied WGPU Presentation Test",
      .initialWidth = 760,
      .initialHeight = 300,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  if (!window.valid() || window.geodeDevice() == nullptr) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }

  std::optional<svg::SVGDocument> document = LoadDonnerSplashDocument();
  if (!document.has_value()) {
    GTEST_SKIP() << "donner_splash.svg not found in runfiles";
  }

  std::optional<svg::SVGElement> target = document->querySelector("#Big_lightning_glow");
  ASSERT_TRUE(target.has_value());
  std::optional<RenderResult::CompositedPreview> preview =
      RenderCompositedPreview(window.geodeDevice(), *document, target->entityHandle().entity());
  ASSERT_TRUE(preview.has_value());
  ASSERT_GE(preview->tiles.size(), 2u) << "Expected splash background plus selected filter layer";

  GlTextureCache textures;
  textures.initialize();
  textures.uploadComposited(*preview);
  ASSERT_GE(textures.tiles().size(), 2u);

  constexpr double kTargetCanvasX = 445.0;
  constexpr double kTargetCanvasY = 196.0;
  constexpr double kReferenceZoom = 1.0;
  constexpr double kZoomedZoom = 2.0;
  constexpr double kReferenceCenterX = 126.0;
  constexpr double kZoomedCenterX = 520.0;
  constexpr double kCenterY = 150.0;
  const ImVec2 referenceOrigin(static_cast<float>(kReferenceCenterX - kTargetCanvasX),
                               static_cast<float>(kCenterY - kTargetCanvasY));
  const ImVec2 zoomedOrigin(static_cast<float>(kZoomedCenterX - kTargetCanvasX * kZoomedZoom),
                            static_cast<float>(kCenterY - kTargetCanvasY * kZoomedZoom));

  window.beginFrame();
  ImDrawList* drawList = ImGui::GetBackgroundDrawList();
  drawList->PushClipRect(ImVec2(8.0f, 8.0f), ImVec2(260.0f, 292.0f),
                         /*intersect_with_current_clip_rect=*/true);
  DrawCompositedTiles(textures, referenceOrigin, kReferenceZoom);
  drawList->PopClipRect();
  drawList->PushClipRect(ImVec2(280.0f, 8.0f), ImVec2(752.0f, 292.0f),
                         /*intersect_with_current_clip_rect=*/true);
  DrawCompositedTiles(textures, zoomedOrigin, kZoomedZoom);
  drawList->PopClipRect();
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();
  WriteDiagnosticBitmap(actual, "zoomed_donner_splash_filtered_layer_presentation.png");

  ASSERT_FALSE(actual.empty());
  const Vector2d readbackFromLogical = ReadbackScale(actual, 760, 300);
  const double referenceCenterLuma =
      MeanLumaAtLogical(actual, readbackFromLogical, kReferenceCenterX, kCenterY, 2.0);
  const double zoomedCenterLuma =
      MeanLumaAtLogical(actual, readbackFromLogical, kZoomedCenterX, kCenterY, 4.0);
  EXPECT_NEAR(zoomedCenterLuma, referenceCenterLuma, 5.0)
      << "A zoomed composited filter layer from donner_splash.svg should preserve center glow "
         "intensity. reference="
      << referenceCenterLuma << " zoomed=" << zoomedCenterLuma;

  const double referenceEdgeLuma =
      MeanLumaAtLogical(actual, readbackFromLogical, kReferenceCenterX + 34.0, kCenterY, 2.0);
  const double zoomedEdgeLuma =
      MeanLumaAtLogical(actual, readbackFromLogical, kZoomedCenterX + 68.0, kCenterY, 4.0);
  EXPECT_NEAR(zoomedEdgeLuma, referenceEdgeLuma, 5.0)
      << "A zoomed composited filter layer from donner_splash.svg should preserve transparent-edge "
         "intensity. reference="
      << referenceEdgeLuma << " zoomed=" << zoomedEdgeLuma;
}
#endif

}  // namespace
}  // namespace donner::editor::gui
