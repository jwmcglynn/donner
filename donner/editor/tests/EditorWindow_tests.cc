#include "donner/editor/gui/EditorWindow.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(DONNER_EDITOR_WGPU)
// Resizing a window is the one thing these cases ask of the window library directly.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "donner/base/Box.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/RunfileGate.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/css/Color.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/CompositorDebugPanel.h"
#include "donner/editor/DocumentSyncController.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorShell.h"
#include "donner/editor/EditorShellPresentation.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/LayersPanel.h"
#include "donner/editor/PenTool.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/TextEditor.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/gui/ImGuiRuntimeRenderer.h"
#include "donner/editor/gui/UiTextureRegistry.h"
#include "donner/editor/tests/CompositorDebugPanelTestAccess.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/Handles.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#if defined(__linux__)
#include "donner/gpu/vulkan/VulkanDevice.h"
#endif
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/StrokeParams.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeGpuWait.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"
#include "donner/svg/renderer/tests/RendererImageTestUtils.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#endif

namespace donner::editor::gui {
namespace {

#if defined(DONNER_EDITOR_WGPU)
using svg::test::Near;
using svg::test::Rgba;

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

int CountGreenPixels(const svg::RendererBitmap& bitmap) {
  int count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const std::array<std::uint8_t, 4> pixel = PixelAt(bitmap, x, y);
      if (pixel[0] < 95u && pixel[1] > 145u && pixel[2] < 90u && pixel[3] > 180u) {
        ++count;
      }
    }
  }
  return count;
}

double MeanLumaAt(const svg::RendererBitmap& bitmap, int centerX, int centerY, int radius) {
  double total = 0.0;
  int count = 0;
  // Use a half-open 2r-by-2r footprint. Inclusive bounds produce a 2r+1
  // footprint, so the 1x and 2x samples cover different source areas (5x5
  // versus 9x9 for radii 2 and 4) and report a false luma shift on gradients.
  for (int y = centerY - radius; y < centerY + radius; ++y) {
    if (y < 0 || y >= bitmap.dimensions.y) {
      continue;
    }
    for (int x = centerX - radius; x < centerX + radius; ++x) {
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

std::optional<svg::SVGDocument> ParseDonnerSplashDocument(std::string_view source) {
  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  auto parsed = svg::parser::SVGParser::ParseSVG(source, warningSink);
  EXPECT_FALSE(parsed.hasError()) << parsed.error();
  if (parsed.hasError()) {
    return std::nullopt;
  }

  svg::SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(892, 512);
  return document;
}

std::string ReadRunfileText(std::string_view path) {
  std::ifstream input{donner::Runfiles::instance().Rlocation(std::string(path))};
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::optional<svg::RendererBitmap> LoadRunfilePngBitmap(std::string_view path) {
  const std::string resolvedPath = donner::Runfiles::instance().Rlocation(std::string(path));
  std::optional<svg::Image> image =
      svg::RendererImageTestUtils::readRgbaImageFromPngFile(resolvedPath.c_str());
  if (!image.has_value()) {
    return std::nullopt;
  }

  svg::RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(image->width, image->height);
  bitmap.rowBytes = image->strideInPixels * 4u;
  bitmap.alphaType = svg::AlphaType::Unpremultiplied;
  bitmap.pixels = std::move(image->data);
  return bitmap;
}

std::optional<LayerTreeRow> FindLayerRow(const LayersPanel& panel, std::string_view displayName) {
  for (const LayerTreeRow& row : panel.model().rows()) {
    if (row.displayName == displayName) {
      return row;
    }
  }
  return std::nullopt;
}

struct ImageDrawRect {
  ImVec2 min;
  ImVec2 max;
};

struct PixelBounds {
  int minX = 0;
  int minY = 0;
  int maxX = 0;
  int maxY = 0;
};

std::optional<PixelBounds> DarkContentBounds(const svg::RendererBitmap& bitmap) {
  std::optional<PixelBounds> bounds;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      if (LumaAt(bitmap, x, y) >= 80.0) {
        continue;
      }

      if (!bounds.has_value()) {
        bounds = PixelBounds{.minX = x, .minY = y, .maxX = x, .maxY = y};
      } else {
        bounds->minX = std::min(bounds->minX, x);
        bounds->minY = std::min(bounds->minY, y);
        bounds->maxX = std::max(bounds->maxX, x);
        bounds->maxY = std::max(bounds->maxY, y);
      }
    }
  }
  return bounds;
}

std::optional<ImageDrawRect> FindTextureDrawRect(const ImDrawList& drawList, ImTextureID texture) {
  for (int cmdIndex = 0; cmdIndex < drawList.CmdBuffer.Size; ++cmdIndex) {
    const ImDrawCmd& cmd = drawList.CmdBuffer[cmdIndex];
    if (cmd.GetTexID() != texture) {
      continue;
    }

    ImVec2 min(FLT_MAX, FLT_MAX);
    ImVec2 max(-FLT_MAX, -FLT_MAX);
    for (unsigned int elemOffset = 0; elemOffset < cmd.ElemCount; ++elemOffset) {
      const int idxOffset = static_cast<int>(cmd.IdxOffset + elemOffset);
      const int vertexIndex =
          static_cast<int>(cmd.VtxOffset) + static_cast<int>(drawList.IdxBuffer[idxOffset]);
      const ImVec2 pos = drawList.VtxBuffer[vertexIndex].pos;
      min.x = std::min(min.x, pos.x);
      min.y = std::min(min.y, pos.y);
      max.x = std::max(max.x, pos.x);
      max.y = std::max(max.y, pos.y);
    }
    return ImageDrawRect{.min = min, .max = max};
  }

  return std::nullopt;
}

void DrawThumbnailCheckerboardForTest(ImDrawList* drawList, const ImVec2& min, const ImVec2& max) {
  constexpr float kCell = 4.0f;
  constexpr ImU32 kLight = IM_COL32(120, 120, 120, 255);
  constexpr ImU32 kDark = IM_COL32(90, 90, 90, 255);
  drawList->AddRectFilled(min, max, kDark);
  int rowParity = 0;
  for (float y = min.y; y < max.y; y += kCell, ++rowParity) {
    const float cellMaxY = std::min(y + kCell, max.y);
    int col = rowParity & 1;
    for (float x = min.x; x < max.x; x += kCell, ++col) {
      if ((col & 1) == 0) {
        continue;
      }
      const float cellMaxX = std::min(x + kCell, max.x);
      drawList->AddRectFilled(ImVec2(x, y), ImVec2(cellMaxX, cellMaxY), kLight);
    }
  }
}

svg::RendererBitmap CropBitmap(const svg::RendererBitmap& source, int x, int y, int width,
                               int height) {
  svg::RendererBitmap crop;
  if (source.empty() || width <= 0 || height <= 0 || x < 0 || y < 0 ||
      x + width > source.dimensions.x || y + height > source.dimensions.y) {
    return crop;
  }

  crop.dimensions = Vector2i(width, height);
  crop.rowBytes = static_cast<std::size_t>(width) * 4u;
  crop.alphaType = source.alphaType;
  crop.pixels.resize(crop.rowBytes * static_cast<std::size_t>(height));
  for (int row = 0; row < height; ++row) {
    const std::size_t sourceOffset =
        static_cast<std::size_t>(y + row) * source.rowBytes + static_cast<std::size_t>(x) * 4u;
    const std::size_t cropOffset = static_cast<std::size_t>(row) * crop.rowBytes;
    std::memcpy(crop.pixels.data() + cropOffset, source.pixels.data() + sourceOffset,
                crop.rowBytes);
  }
  return crop;
}

svg::RendererBitmap CropLogicalRect(const svg::RendererBitmap& source,
                                    const Vector2d& readbackFromLogical,
                                    const ImageDrawRect& rect) {
  const int x = static_cast<int>(std::lround(rect.min.x * readbackFromLogical.x));
  const int y = static_cast<int>(std::lround(rect.min.y * readbackFromLogical.y));
  const int width =
      static_cast<int>(std::lround((rect.max.x - rect.min.x) * readbackFromLogical.x));
  const int height =
      static_cast<int>(std::lround((rect.max.y - rect.min.y) * readbackFromLogical.y));
  return CropBitmap(source, x, y, width, height);
}

/// Registers a Geode snapshot's runtime texture as a UI texture for this object's lifetime, so a
/// test draws it the way editor code does: through an identifier the renderer resolves, never a
/// backend address.
class ScopedUiTexture {
public:
  /// Registers \p texture with \p alphaMode on the device the UI renderer draws with.
  /// @param texture Snapshot to register; must be a Geode snapshot.
  /// @param alphaMode Alpha interpretation of the sampled texels.
  ScopedUiTexture(const svg::RendererTextureSnapshot& texture, UiTextureAlphaMode alphaMode)
      : registry_(CurrentUiTextureRegistry()) {
    if (registry_ == nullptr || CurrentImGuiRuntimeRenderer() == nullptr ||
        texture.backend() != svg::RendererTextureSnapshotBackend::Geode) {
      return;
    }
    // Registrations belong to the device the UI is drawn on, which is not necessarily the device
    // the snapshot's pixels were rendered on.
    gpu::Device& device = CurrentImGuiRuntimeRenderer()->device();
    const auto& geodeTexture = static_cast<const svg::RendererGeodeTextureSnapshot&>(texture);
    const Vector2i dimensions = geodeTexture.dimensions();
    const gpu::Texture* runtimeTexture = geodeTexture.runtimeTexture();
    if (runtimeTexture == nullptr) {
      return;
    }
    // A texture of the drawing device is registered directly; one rendered on another device is
    // registered here from the export its producer took, which is what makes it nameable on this
    // one.
    if (runtimeTexture->deviceId() != device.deviceId()) {
      const gpu::TextureExport* exported = geodeTexture.textureExport();
      if (exported == nullptr) {
        return;
      }
      gpu::Result<gpu::Texture> imported = geode::RegisterOrderedTexture(device, *exported);
      if (imported.hasError()) {
        return;
      }
      imported_ = std::move(imported).result();
      runtimeTexture = &imported_;
    }
    gpu::Result<gpu::TextureView> view =
        device.createTextureView(*runtimeTexture, gpu::TextureViewDescriptor{"testUiTexture"});
    if (view.hasError()) {
      return;
    }
    gpu::Result<UiTextureId> registered = registry_->registerTexture(UiTextureDescriptor{
        view.result(),
        {static_cast<uint32_t>(dimensions.x), static_cast<uint32_t>(dimensions.y)},
        alphaMode});
    if (registered.hasError()) {
      return;
    }
    view_ = std::move(view).result();
    id_ = registered.result();
  }

  ~ScopedUiTexture() {
    if (registry_ != nullptr && id_.isValid()) {
      const gpu::Status retired = registry_->retire(id_);
      (void)retired;
    }
  }

  ScopedUiTexture(const ScopedUiTexture&) = delete;
  ScopedUiTexture& operator=(const ScopedUiTexture&) = delete;

  /// The identifier draw data carries for this texture, zero when registration failed.
  ImTextureID id() const { return id_.imTextureId(); }

private:
  UiTextureRegistry* registry_ = nullptr;
  gpu::Texture imported_;
  gpu::TextureView view_;
  UiTextureId id_;
};

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

std::optional<RenderResult> WaitForRenderResult(AsyncRenderer& asyncRenderer) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    std::optional<RenderResult> result = asyncRenderer.pollResult();
    if (result.has_value()) {
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return std::nullopt;
}

std::optional<RenderResult> RenderSelectedPromotedPreview(
    AsyncRenderer& asyncRenderer, svg::Renderer& renderer, EditorApp& app, Entity selectedEntity,
    std::uint64_t version, bool forceLayerRasterization,
    std::optional<EditorRasterViewport> rasterViewport = std::nullopt) {
  RenderRequest request(renderer, app.document().document());
  request.version = version;
  request.documentGeneration = app.document().documentGeneration();
  request.structuralRemap = app.document().consumePendingStructuralRemap();
  request.selectedEntity = selectedEntity;
  request.dragPreview = RenderRequest::DragPreview{
      .entity = selectedEntity,
      .interactionKind = svg::compositor::InteractionHint::Selection,
      .forceLayerRasterization = forceLayerRasterization,
  };
  if (rasterViewport.has_value()) {
    request.rasterViewport = *rasterViewport;
  }
  asyncRenderer.requestRender(request);
  return WaitForRenderResult(asyncRenderer);
}

const RenderResult::CompositedTile* FindLayerTile(const RenderResult::CompositedPreview& preview,
                                                  Entity layerEntity) {
  const auto it = std::find_if(preview.tiles.begin(), preview.tiles.end(),
                               [layerEntity](const RenderResult::CompositedTile& tile) {
                                 return tile.kind == RenderResult::CompositedTile::Kind::Layer &&
                                        tile.layerEntity == layerEntity;
                               });
  return it != preview.tiles.end() ? &(*it) : nullptr;
}

ViewportState PenFillReplayViewport() {
  ViewportState viewport;
  viewport.paneOrigin = Vector2d(568.0, 29.0);
  viewport.paneSize = Vector2d(604.0, 863.0);
  viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 892.0, 512.0);
  viewport.devicePixelRatio = 2.0;
  viewport.zoom = 1.0;
  viewport.panDocPoint = Vector2d(446.0, 256.0);
  viewport.panScreenPoint = Vector2d(870.0, 460.5);
  return viewport;
}

void ExpectTileEnclosesPathBounds(const RenderResult::CompositedTile& tile,
                                  const Box2d& pathBoundsDoc) {
  const Box2d tileBoundsDoc(tile.canvasOffsetDoc, tile.canvasOffsetDoc + tile.bitmapDimsDoc);
  EXPECT_LE(tileBoundsDoc.topLeft.x, pathBoundsDoc.topLeft.x);
  EXPECT_LE(tileBoundsDoc.topLeft.y, pathBoundsDoc.topLeft.y);
  EXPECT_GE(tileBoundsDoc.bottomRight.x, pathBoundsDoc.bottomRight.x);
  EXPECT_GE(tileBoundsDoc.bottomRight.y, pathBoundsDoc.bottomRight.y);
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

  return RenderCompositedPreview(device, document, glow->unsafeEntityHandle().entity());
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
class EditorShellUiFontTest : public testing::TestWithParam<double> {};

TEST_P(EditorShellUiFontTest, DefaultUiFontKeepsItsLogicalSizeAfterRendererStartup) {
  EditorWindow window(EditorWindowOptions{
      .title = "Editor font scale regression",
      .initialWidth = 960,
      .initialHeight = 640,
      .visible = false,
      .offscreen = true,
      .forceOffscreenRenderTarget = true,
      .offscreenContentScale = GetParam(),
      .enableFramebufferReadback = true,
  });
  ASSERT_THAT(window.valid(), testing::IsTrue());
  SCOPED_TRACE(testing::Message() << "display scale: " << window.displayScale());
  EditorShellOptions options;
  options.initialSource =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="640" height="400"/>)svg";
  EditorShell shell(window, std::move(options));
  ASSERT_THAT(shell.valid(), testing::IsTrue());
  ASSERT_THAT(window.editorFonts().complete(), testing::IsTrue());

  // ImGui truncates baked sizes to whole physical pixels before applying FontGlobalScale.
  const float expectedUiSize =
      static_cast<float>(std::floor(15.0 * window.displayScale()) / window.displayScale());
  for (int frame = 0; frame < 2; ++frame) {
    window.beginFrame();
    EXPECT_THAT(ImGui::GetFont(), testing::Eq(window.editorFonts().uiRegular));
    EXPECT_THAT(ImGui::GetFontSize(), testing::FloatEq(expectedUiSize));
    ImGui::PushFont(window.editorFonts().uiBold);
    EXPECT_THAT(ImGui::GetFontSize(), testing::FloatEq(expectedUiSize));
    ImGui::PopFont();
    ImGui::PushFont(window.editorFonts().code);
    EXPECT_THAT(ImGui::GetFontSize(), testing::FloatEq(14.0f));
    ImGui::PopFont();
    shell.runFrame();
    const svg::RendererBitmap bitmap = window.endFrameAndReadPixels();
    EXPECT_THAT(bitmap.empty(), testing::IsFalse());
    if (frame == 1) {
      WriteDiagnosticBitmap(bitmap, "editor_ui_scale_" + std::to_string(GetParam()) + ".png");
    }
  }
}

INSTANTIATE_TEST_SUITE_P(DisplayScales, EditorShellUiFontTest, testing::Values(1.0, 1.5, 2.0));

TEST(EditorWindowTest, SurfaceStatusesRouteToTheirOwnRecovery) {
  using Action = internal::SurfaceFrameAction;

  EXPECT_EQ(internal::SurfaceFrameActionFor(gpu::SurfaceStatus::Success), Action::Draw);

  // A configuration that no longer matches its window is followed with a new one, which is what
  // the resize path already does, rather than costing the frame.
  EXPECT_EQ(internal::SurfaceFrameActionFor(gpu::SurfaceStatus::Outdated),
            Action::ReconfigureAndRetry);

  // Neither a platform object that is gone nor a lost device recovers by reconfiguring, so the
  // surface is given up instead of acquired from again.
  EXPECT_EQ(internal::SurfaceFrameActionFor(gpu::SurfaceStatus::Lost), Action::Release);
  EXPECT_EQ(internal::SurfaceFrameActionFor(gpu::SurfaceStatus::DeviceLost), Action::Release);

  // Nothing became available in time; the next frame asks again.
  EXPECT_EQ(internal::SurfaceFrameActionFor(gpu::SurfaceStatus::Timeout), Action::Skip);
}

TEST(EditorWindowTest, SurfaceStatusesKeepTheirBrowserRetryBuckets) {
  using Failure = internal::WgpuSurfaceFailureKind;

  EXPECT_EQ(internal::WgpuSurfaceFailureKindFor(gpu::SurfaceStatus::Timeout), Failure::Timeout);
  EXPECT_EQ(internal::WgpuSurfaceFailureKindFor(gpu::SurfaceStatus::Outdated),
            Failure::OutdatedOrLost);
  EXPECT_EQ(internal::WgpuSurfaceFailureKindFor(gpu::SurfaceStatus::Lost), Failure::OutdatedOrLost);

  // A lost device must not rearm the event-driven browser loop.
  EXPECT_EQ(internal::WgpuSurfaceFailureKindFor(gpu::SurfaceStatus::DeviceLost), Failure::Fatal);
  EXPECT_EQ(internal::WgpuSurfaceRetryDecisionFor(
                internal::WgpuSurfaceFailureKindFor(gpu::SurfaceStatus::DeviceLost), 0u),
            (internal::WgpuSurfaceRetryDecision{}));
}

/// A frame handle carrying no device-alive token. The frame-acquisition orchestration only checks
/// whether a frame came back and passes the handle along, so a handle that is merely valid is all
/// a scripted surface needs to stand in for one; with no token to resolve against, it releases
/// nothing when it goes.
gpu::Texture FakeFrameTexture() {
  return gpu::Texture::CreateForBackend(/*slotIndex=*/0, /*generation=*/1, /*deviceId=*/1);
}

/// What the orchestration did to a scripted surface.
struct SurfaceCalls {
  int acquires = 0;            //!< Frames asked for.
  int abandons = 0;            //!< Frames handed back without being shown.
  int configures = 0;          //!< Configurations applied or refused.
  int shutdowns = 0;           //!< Times the surface was given up.
  Vector2i lastConfigureSize;  //!< Extent of the most recent configuration.

  bool operator==(const SurfaceCalls&) const = default;
};

std::ostream& operator<<(std::ostream& os, const SurfaceCalls& calls) {
  return os << "{acquires=" << calls.acquires << ", abandons=" << calls.abandons
            << ", configures=" << calls.configures << ", shutdowns=" << calls.shutdowns
            << ", lastConfigureSize=" << calls.lastConfigureSize << "}";
}

/// Hands back the statuses its script names, so the frame loop's recovery can be driven without a
/// window or a device behind it.
class ScriptedSurface final : public internal::PresentationSurface {
public:
  /// @param script Statuses to report, in order; the last one repeats once the script runs out.
  /// @param calls Record of what the orchestration did; outlives this surface.
  ScriptedSurface(std::vector<gpu::SurfaceStatus> script, SurfaceCalls* calls)
      : script_(std::move(script)), calls_(calls) {}

  /// Makes every configuration refuse, standing in for a window the surface cannot follow.
  void refuseConfiguration() { configureSucceeds_ = false; }

  bool attachToWindow(const wgpu::Instance&, GLFWwindow*) override { return true; }
#ifndef __APPLE__
  wgpu::Surface adapterSelectionSurface() const override { return {}; }
#endif
  bool chooseConfiguration(const wgpu::Adapter&, bool) override { return true; }
  bool attachToDevice(geode::GeodeDevice&) override { return true; }

  bool configure(int width, int height) override {
    ++calls_->configures;
    calls_->lastConfigureSize = Vector2i(width, height);
    return configureSucceeds_;
  }

  internal::AcquiredFrame acquire() override {
    ++calls_->acquires;
    const gpu::SurfaceStatus status =
        nextStatus_ < script_.size() ? script_[nextStatus_] : script_.back();
    ++nextStatus_;
    // A surface that has drifted out of date still hands back a usable frame; the statuses that
    // report no frame at all hand back nothing.
    const bool carriesFrame =
        status == gpu::SurfaceStatus::Success || status == gpu::SurfaceStatus::Outdated;
    return internal::AcquiredFrame{carriesFrame ? FakeFrameTexture() : gpu::Texture(), status};
  }

  void present() override {}
  void abandon() override { ++calls_->abandons; }
  void shutdown() override { ++calls_->shutdowns; }
  gpu::TextureFormat format() const override { return gpu::TextureFormat::BGRA8Unorm; }
  gpu::TextureUsage usage() const override { return gpu::TextureUsage::RenderAttachment; }
  bool premultipliedAlpha() const override { return false; }

private:
  std::vector<gpu::SurfaceStatus> script_;
  SurfaceCalls* calls_;
  std::size_t nextStatus_ = 0;
  bool configureSucceeds_ = true;
};

/// Framebuffer the orchestration is asked to present to.
const Vector2i kFrameSizePx(1280, 720);

/// A scripted surface reporting \p statuses, as the orchestration takes it.
/// @param statuses Statuses to report, in order. @param calls Record to fill in.
std::unique_ptr<internal::PresentationSurface> ScriptedSurfaceReporting(
    std::vector<gpu::SurfaceStatus> statuses, SurfaceCalls* calls) {
  return std::make_unique<ScriptedSurface>(std::move(statuses), calls);
}

TEST(EditorWindowTest, AMinimizedWindowSkipsTheFrameWithoutHoldingOneOpen) {
  SurfaceCalls calls;
  std::unique_ptr<internal::PresentationSurface> surface =
      ScriptedSurfaceReporting({gpu::SurfaceStatus::Success}, &calls);
  Vector2i configuredPx(640, 480);

  // A minimized window reports a framebuffer with no texels in it, which is not an extent a
  // surface can be configured for or hand out a frame of.
  const internal::PresentationFrameOutcome outcome =
      internal::AcquirePresentationFrame(surface, Vector2i::Zero(), configuredPx, nullptr);

  EXPECT_FALSE(outcome.texture.isValid()) << "there is no framebuffer to draw into";
  EXPECT_EQ(outcome.status, gpu::SurfaceStatus::Success)
      << "nothing failed; there was simply no frame to ask for";
  EXPECT_FALSE(outcome.released) << "the surface is still the window's; only this frame is gone";
  EXPECT_FALSE(outcome.markDeviceLost) << "a window with nothing to draw into is not a lost device";
  EXPECT_NE(surface, nullptr);
  EXPECT_EQ(calls, (SurfaceCalls{.acquires = 0,
                                 .abandons = 0,
                                 .configures = 0,
                                 .shutdowns = 0,
                                 .lastConfigureSize = Vector2i::Zero()}))
      << "a frame asked for and then handed straight back is an acquisition the window never "
         "needed to open";
  EXPECT_EQ(configuredPx, Vector2i(640, 480))
      << "the surface still matches the extent it was configured for";
}

TEST(EditorWindowTest, ATimedOutFrameIsSkippedAndTheSurfaceKept) {
  SurfaceCalls calls;
  std::unique_ptr<internal::PresentationSurface> surface =
      ScriptedSurfaceReporting({gpu::SurfaceStatus::Timeout}, &calls);
  Vector2i configuredPx = kFrameSizePx;

  const internal::PresentationFrameOutcome outcome =
      internal::AcquirePresentationFrame(surface, kFrameSizePx, configuredPx, nullptr);

  EXPECT_FALSE(outcome.texture.isValid());
  EXPECT_EQ(outcome.status, gpu::SurfaceStatus::Timeout);
  EXPECT_FALSE(outcome.released) << "nothing became available in time; the surface is still fine";
  EXPECT_FALSE(outcome.markDeviceLost);
  EXPECT_NE(surface, nullptr);
  EXPECT_EQ(configuredPx, kFrameSizePx) << "a dropped frame does not reconfigure the surface";
  EXPECT_EQ(calls, (SurfaceCalls{.acquires = 1,
                                 .abandons = 1,
                                 .configures = 0,
                                 .shutdowns = 0,
                                 .lastConfigureSize = Vector2i::Zero()}))
      << "the frame that never came is handed back once and nothing else happens";
}

TEST(EditorWindowTest, AnOutdatedFrameFollowsTheWindowAndIsRetriedOnce) {
  SurfaceCalls calls;
  std::unique_ptr<internal::PresentationSurface> surface =
      ScriptedSurfaceReporting({gpu::SurfaceStatus::Outdated, gpu::SurfaceStatus::Success}, &calls);
  Vector2i configuredPx(640, 480);

  const internal::PresentationFrameOutcome outcome =
      internal::AcquirePresentationFrame(surface, kFrameSizePx, configuredPx, nullptr);

  EXPECT_TRUE(outcome.texture.isValid())
      << "following the window produced a frame instead of costing one";
  EXPECT_EQ(outcome.status, gpu::SurfaceStatus::Success);
  EXPECT_FALSE(outcome.released);
  EXPECT_EQ(configuredPx, kFrameSizePx);
  EXPECT_EQ(calls, (SurfaceCalls{.acquires = 2,
                                 .abandons = 1,
                                 .configures = 1,
                                 .shutdowns = 0,
                                 .lastConfigureSize = kFrameSizePx}));
}

TEST(EditorWindowTest, AnOutdatedFrameIsDroppedWhenTheWindowCannotBeFollowed) {
  SurfaceCalls calls;
  auto scripted = std::make_unique<ScriptedSurface>(
      std::vector<gpu::SurfaceStatus>{gpu::SurfaceStatus::Outdated}, &calls);
  scripted->refuseConfiguration();
  std::unique_ptr<internal::PresentationSurface> surface = std::move(scripted);
  Vector2i configuredPx(640, 480);

  const internal::PresentationFrameOutcome outcome =
      internal::AcquirePresentationFrame(surface, kFrameSizePx, configuredPx, nullptr);

  EXPECT_FALSE(outcome.texture.isValid());
  EXPECT_EQ(outcome.status, gpu::SurfaceStatus::Outdated);
  EXPECT_FALSE(outcome.released) << "the surface is still usable; only this frame was dropped";
  EXPECT_NE(surface, nullptr);
  EXPECT_EQ(configuredPx, Vector2i::Zero())
      << "the recorded extent is cleared so the next frame configures again";
  EXPECT_EQ(calls.acquires, 1) << "a refused configuration is not acquired against";
}

TEST(EditorWindowTest, ALostSurfaceIsRebuiltOnceAndKeepsRendering) {
  SurfaceCalls lostCalls;
  SurfaceCalls rebuiltCalls;
  std::unique_ptr<internal::PresentationSurface> surface =
      ScriptedSurfaceReporting({gpu::SurfaceStatus::Lost}, &lostCalls);
  Vector2i configuredPx(640, 480);
  int rebuilds = 0;

  const internal::PresentationFrameOutcome outcome =
      internal::AcquirePresentationFrame(surface, kFrameSizePx, configuredPx, [&] {
        ++rebuilds;
        return ScriptedSurfaceReporting({gpu::SurfaceStatus::Success}, &rebuiltCalls);
      });

  EXPECT_EQ(rebuilds, 1);
  EXPECT_TRUE(outcome.texture.isValid());
  EXPECT_EQ(outcome.status, gpu::SurfaceStatus::Success);
  EXPECT_FALSE(outcome.released) << "a rebuilt surface keeps the window presenting";
  EXPECT_NE(surface, nullptr);
  EXPECT_EQ(configuredPx, kFrameSizePx);
  EXPECT_EQ(lostCalls.shutdowns, 1) << "the lost surface is given up once its replacement exists";
  EXPECT_EQ(rebuiltCalls.acquires, 1);
}

TEST(EditorWindowTest, ALostSurfaceThatCannotBeRebuiltIsGivenUp) {
  SurfaceCalls calls;
  std::unique_ptr<internal::PresentationSurface> surface =
      ScriptedSurfaceReporting({gpu::SurfaceStatus::Lost}, &calls);
  Vector2i configuredPx(640, 480);
  int rebuilds = 0;

  const internal::PresentationFrameOutcome outcome = internal::AcquirePresentationFrame(
      surface, kFrameSizePx, configuredPx, [&]() -> std::unique_ptr<internal::PresentationSurface> {
        ++rebuilds;
        return nullptr;
      });

  EXPECT_EQ(rebuilds, 1) << "rebuilding is attempted once, not repeatedly";
  EXPECT_FALSE(outcome.texture.isValid());
  EXPECT_EQ(outcome.status, gpu::SurfaceStatus::Lost);
  EXPECT_TRUE(outcome.released);
  EXPECT_FALSE(outcome.markDeviceLost) << "the device is fine; only the surface was lost";
  EXPECT_EQ(surface, nullptr);
  EXPECT_EQ(configuredPx, Vector2i::Zero());
  EXPECT_EQ(calls.shutdowns, 1);
}

TEST(EditorWindowTest, ALostCanvasSurfaceCanRecoverOnTheNextFrame) {
  SurfaceCalls lostCalls;
  SurfaceCalls recoveredCalls;
  std::unique_ptr<internal::PresentationSurface> surface =
      ScriptedSurfaceReporting({gpu::SurfaceStatus::Lost}, &lostCalls);
  Vector2i configuredPx = kFrameSizePx;

  const internal::PresentationFrameOutcome first = internal::AcquirePresentationFrame(
      surface, kFrameSizePx, configuredPx,
      []() -> std::unique_ptr<internal::PresentationSurface> { return nullptr; });
  ASSERT_TRUE(first.released);
  ASSERT_EQ(surface, nullptr);

  const internal::PresentationFrameOutcome next = internal::AcquirePresentationFrame(
      surface, kFrameSizePx, configuredPx,
      [&] { return ScriptedSurfaceReporting({gpu::SurfaceStatus::Success}, &recoveredCalls); });

  EXPECT_TRUE(next.texture.isValid()) << "a transient rebuild failure must not strand the canvas";
  EXPECT_EQ(next.status, gpu::SurfaceStatus::Success);
  EXPECT_FALSE(next.released);
  EXPECT_NE(surface, nullptr);
  EXPECT_EQ(configuredPx, kFrameSizePx);
  EXPECT_EQ(recoveredCalls.acquires, 1);
}

TEST(EditorWindowTest, ASecondLostAfterRebuildingGivesTheSurfaceUp) {
  SurfaceCalls lostCalls;
  SurfaceCalls rebuiltCalls;
  std::unique_ptr<internal::PresentationSurface> surface =
      ScriptedSurfaceReporting({gpu::SurfaceStatus::Lost}, &lostCalls);
  Vector2i configuredPx(640, 480);
  int rebuilds = 0;

  const internal::PresentationFrameOutcome outcome =
      internal::AcquirePresentationFrame(surface, kFrameSizePx, configuredPx, [&] {
        ++rebuilds;
        return ScriptedSurfaceReporting({gpu::SurfaceStatus::Lost}, &rebuiltCalls);
      });

  EXPECT_EQ(rebuilds, 1) << "the replacement is not itself replaced; the loss has settled";
  EXPECT_TRUE(outcome.released);
  EXPECT_EQ(outcome.status, gpu::SurfaceStatus::Lost);
  EXPECT_EQ(surface, nullptr);
  EXPECT_EQ(rebuiltCalls.shutdowns, 1);
}

TEST(EditorWindowTest, ALostDeviceIsTerminalAndIsReportedToTheRenderers) {
  SurfaceCalls calls;
  std::unique_ptr<internal::PresentationSurface> surface =
      ScriptedSurfaceReporting({gpu::SurfaceStatus::DeviceLost}, &calls);
  Vector2i configuredPx(640, 480);
  int rebuilds = 0;

  const internal::PresentationFrameOutcome outcome =
      internal::AcquirePresentationFrame(surface, kFrameSizePx, configuredPx, [&] {
        ++rebuilds;
        return ScriptedSurfaceReporting({gpu::SurfaceStatus::Success}, &calls);
      });

  EXPECT_EQ(rebuilds, 0) << "a fresh surface does not bring back a lost device";
  EXPECT_FALSE(outcome.texture.isValid());
  EXPECT_TRUE(outcome.released);
  EXPECT_TRUE(outcome.markDeviceLost);
  EXPECT_EQ(surface, nullptr);
  EXPECT_EQ(configuredPx, Vector2i::Zero());
}

/// A device whose surface hooks follow a script, so the editor's presentation surface can be
/// driven without a window, a platform object, or a GPU behind it.
///
/// Everything that is not presentation is accepted and ignored: these cases are about which
/// runtime surface operations one frame performs, in what order, and what the runtime does with
/// the frame handle afterwards.
class ScriptedSurfaceDevice final : public gpu::Device {
public:
  /// Submissions complete instantly; nothing here waits on one.
  uint64_t completedSerial() const override { return lastSubmittedSerial(); }

  /// What the surface reports while handing over a frame.
  gpu::SurfaceStatus acquireStatus = gpu::SurfaceStatus::Success;
  /// Formats the surface reports its frames may take.
  std::vector<gpu::TextureFormat> formats{gpu::TextureFormat::BGRA8Unorm};
  /// Usage flags the surface reports its frames may carry.
  gpu::TextureUsage usages = gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc;
  /// Alpha compositing the surface reports it supports.
  std::vector<gpu::SurfaceAlphaMode> alphaModes{gpu::SurfaceAlphaMode::Opaque};

  /// Platform objects surfaces were created for, in order.
  std::vector<gpu::NativeSurfaceKind> createdSurfaces;
  /// Extents the surface was configured for, in order.
  std::vector<gpu::Extent2d> configuredSizes;
  /// Configuration the most recent \ref configure applied.
  gpu::SurfaceConfiguration lastConfiguration;
  int presentCalls = 0;         //!< Frames handed to the platform.
  int abandonCalls = 0;         //!< Frames handed back without being shown.
  int destroySurfaceCalls = 0;  //!< Surfaces whose platform state was released.

protected:
  gpu::Status onCreateSurface(uint32_t, const gpu::SurfaceDescriptor& descriptor) override {
    createdSurfaces.push_back(descriptor.native.kind);
    return gpu::OkStatus();
  }

  gpu::Result<gpu::SurfaceCapabilities> onSurfaceCapabilities(uint32_t) const override {
    return gpu::SurfaceCapabilities{formats, usages, {gpu::PresentMode::Fifo}, alphaModes};
  }

  gpu::Status onConfigureSurface(uint32_t,
                                 const gpu::SurfaceConfiguration& configuration) override {
    configuredSizes.push_back(configuration.size);
    lastConfiguration = configuration;
    backendHasFrame_ = false;
    return gpu::OkStatus();
  }

  gpu::Result<gpu::SurfaceStatus> onAcquireCurrentTexture(uint32_t, uint32_t) override {
    // The platform holds its own frame and holds exactly one, so a backend still holding the
    // previous frame refuses to hand out another. The runtime refuses that first, so this models
    // the platform faithfully rather than being the refusal any case here observes.
    if (backendHasFrame_) {
      return gpu::GpuError{gpu::GpuErrorType::InvalidState,
                           "the backend is still holding the frame it handed out"};
    }
    if (acquireStatus == gpu::SurfaceStatus::Success ||
        acquireStatus == gpu::SurfaceStatus::Outdated) {
      backendHasFrame_ = true;
    }
    return acquireStatus;
  }

  gpu::Result<gpu::SurfaceStatus> onPresentSurface(uint32_t) override {
    ++presentCalls;
    backendHasFrame_ = false;
    return gpu::SurfaceStatus::Success;
  }

  void onAbandonCurrentTexture(uint32_t) override {
    ++abandonCalls;
    backendHasFrame_ = false;
  }

  void onDestroySurface(uint32_t) override { ++destroySurfaceCalls; }

  // The operations these cases do not model are accepted and ignored.
  gpu::Status onCreateBuffer(uint32_t, const gpu::BufferDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateTexture(uint32_t, const gpu::TextureDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateTextureView(uint32_t, uint32_t, const gpu::TextureViewDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateSampler(uint32_t, const gpu::SamplerDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateBindGroupLayout(uint32_t, const gpu::BindGroupLayoutDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateBindGroup(uint32_t, const gpu::BindGroupDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreatePipelineLayout(uint32_t, const gpu::PipelineLayoutDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateShaderModule(uint32_t, const gpu::ShaderModuleDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateRenderPipeline(uint32_t, const gpu::RenderPipelineDescriptor&) override {
    return gpu::OkStatus();
  }
  gpu::Status onCreateComputePipeline(uint32_t, const gpu::ComputePipelineDescriptor&) override {
    return gpu::OkStatus();
  }
  void onDestroyResource(std::string_view, uint32_t) override {}
  gpu::Status onWriteBuffer(uint32_t, uint64_t, std::span<const uint8_t>) override {
    return gpu::OkStatus();
  }
  gpu::Status onWriteTexture(uint32_t, std::span<const uint8_t>, const gpu::TexelCopyBufferLayout&,
                             const gpu::Extent2d&, const gpu::Origin2d&) override {
    return gpu::OkStatus();
  }
  gpu::Status onSubmit(uint64_t, std::span<const gpu::SubmittedCommandBuffer>) override {
    return gpu::OkStatus();
  }

private:
  /// Whether the platform is still holding the frame this device handed out.
  bool backendHasFrame_ = false;
};

/// Drives the editor's runtime presentation surface against a scripted device.
class RuntimePresentationSurfaceTest : public testing::Test {
protected:
  /// A platform object the scripted device never dereferences, named the way every platform but
  /// macOS names the surface object its window library already made.
  static gpu::NativeSurfaceHandle NativeHandle() {
    gpu::NativeSurfaceHandle native;
    native.kind = gpu::NativeSurfaceKind::EmbedderSurface;
    native.window = 0x1234;
    return native;
  }

  /// Attaches \ref surface_ to \ref device_ and configures it, failing the case when either step
  /// refuses. @param sizePx Framebuffer extent to configure for.
  void attachAndConfigure(Vector2i sizePx = Vector2i(1280, 720)) {
    ASSERT_TRUE(surface_.attachToRuntime(device_, NativeHandle(), gpu::TextureFormat::BGRA8Unorm,
                                         /*enableReadback=*/false));
    ASSERT_TRUE(surface_.configure(sizePx.x, sizePx.y));
  }

  /// A view of \p texture, which is how a use of a frame handle is checked.
  /// @param texture Frame handle to use.
  gpu::Result<gpu::TextureView> useFrame(const gpu::Texture& texture) {
    return device_.createTextureView(texture, gpu::TextureViewDescriptor{"editorFrame"});
  }

  ScriptedSurfaceDevice device_;
  internal::RuntimePresentationSurface surface_;
};

TEST_F(RuntimePresentationSurfaceTest, FollowsAResizeByReconfiguringTheSameSurface) {
  ASSERT_NO_FATAL_FAILURE(attachAndConfigure(Vector2i(1280, 720)));
  internal::AcquiredFrame first = surface_.acquire();
  ASSERT_THAT(first.status, testing::Eq(gpu::SurfaceStatus::Success));
  surface_.present();

  EXPECT_TRUE(surface_.configure(800, 600));
  internal::AcquiredFrame resized = surface_.acquire();
  EXPECT_THAT(resized.status, testing::Eq(gpu::SurfaceStatus::Success));
  EXPECT_TRUE(resized.texture.isValid());

  EXPECT_THAT(device_.createdSurfaces,
              testing::ElementsAre(gpu::NativeSurfaceKind::EmbedderSurface))
      << "a resize follows the window with a new configuration, not a new surface";
  EXPECT_THAT(device_.configuredSizes,
              testing::ElementsAre(gpu::Extent2d{1280, 720}, gpu::Extent2d{800, 600}));
  EXPECT_EQ(device_.destroySurfaceCalls, 0) << "the surface it reconfigured is the one it had";
}

TEST_F(RuntimePresentationSurfaceTest, ReconfiguringHandsBackAFrameThatWasStillOutstanding) {
  ASSERT_NO_FATAL_FAILURE(attachAndConfigure());
  internal::AcquiredFrame frame = surface_.acquire();
  ASSERT_TRUE(frame.texture.isValid());

  EXPECT_TRUE(surface_.configure(800, 600)) << "a resize is a configuration the surface accepts";

  EXPECT_EQ(device_.abandonCalls, 1)
      << "the platform is holding that frame and holds exactly one, so reconfiguring gives it "
         "back rather than leaving the next acquire to be refused";
  EXPECT_THAT(useFrame(frame.texture), gpu::IsGpuError(gpu::GpuErrorType::InvalidHandle))
      << "the frame described a surface that no longer exists in that shape";
}

TEST_F(RuntimePresentationSurfaceTest, AConfigurationThatWasRefusedStillLeavesNoFrameHeld) {
  ASSERT_NO_FATAL_FAILURE(attachAndConfigure());
  internal::AcquiredFrame frame = surface_.acquire();
  ASSERT_TRUE(frame.texture.isValid());

  // A window with no framebuffer has no extent to configure for, which is the refusal a window
  // can actually arrive at.
  EXPECT_FALSE(surface_.configure(0, 0));

  EXPECT_EQ(device_.abandonCalls, 1)
      << "the outstanding frame goes back before a new configuration is asked for, so a refused "
         "one does not leave the platform holding the only frame it has to give";
  EXPECT_THAT(useFrame(frame.texture), gpu::IsGpuError(gpu::GpuErrorType::InvalidHandle));

  EXPECT_TRUE(surface_.configure(800, 600));
  internal::AcquiredFrame next = surface_.acquire();
  EXPECT_THAT(next.status, testing::Eq(gpu::SurfaceStatus::Success));
  EXPECT_TRUE(next.texture.isValid()) << "the window recovers on the next extent it can present";
}

TEST_F(RuntimePresentationSurfaceTest, APresentedFrameIsNoLongerTheCallersToDrawInto) {
  ASSERT_NO_FATAL_FAILURE(attachAndConfigure());
  internal::AcquiredFrame frame = surface_.acquire();
  ASSERT_TRUE(frame.texture.isValid());

  surface_.present();

  EXPECT_EQ(device_.presentCalls, 1);
  EXPECT_THAT(useFrame(frame.texture), gpu::IsGpuError(gpu::GpuErrorType::InvalidHandle))
      << "the platform owns the frame once it has been handed over";
}

TEST_F(RuntimePresentationSurfaceTest, AnAbandonedFrameIsNoLongerTheCallersToDrawInto) {
  ASSERT_NO_FATAL_FAILURE(attachAndConfigure());
  internal::AcquiredFrame frame = surface_.acquire();
  ASSERT_TRUE(frame.texture.isValid());

  surface_.abandon();

  EXPECT_EQ(device_.abandonCalls, 1);
  EXPECT_EQ(device_.presentCalls, 0) << "a frame the window decided not to draw is not shown";
  EXPECT_THAT(useFrame(frame.texture), gpu::IsGpuError(gpu::GpuErrorType::InvalidHandle));
}

TEST_F(RuntimePresentationSurfaceTest, AskingForASecondFrameWithoutResolvingTheFirstIsRefused) {
  ASSERT_NO_FATAL_FAILURE(attachAndConfigure());
  internal::AcquiredFrame first = surface_.acquire();
  ASSERT_TRUE(first.texture.isValid());

  internal::AcquiredFrame second = surface_.acquire();

  EXPECT_FALSE(second.texture.isValid());
  EXPECT_THAT(second.status, testing::Eq(gpu::SurfaceStatus::Lost))
      << "it is this surface that cannot serve the frame, not the device, so the window spends a "
         "rebuild on it rather than giving the device up for good";
  EXPECT_THAT(useFrame(first.texture), gpu::HasResult())
      << "the frame already handed out is untouched by the refusal";
}

TEST_F(RuntimePresentationSurfaceTest, PresentingWithNoFrameHeldDoesNothing) {
  ASSERT_NO_FATAL_FAILURE(attachAndConfigure());

  // The frame loop presents unconditionally on its way out, including the frames it skipped.
  surface_.present();
  surface_.abandon();

  EXPECT_EQ(device_.presentCalls, 0);
  EXPECT_EQ(device_.abandonCalls, 0);
}

#ifdef __APPLE__
/// A Metal layer presents BGRA8Unorm whichever device draws into it, so the window settles the
/// format its renderer compiles pipelines for without an adapter to ask. A native device's
/// selection produces none, and the format is still checked against what the surface reports once
/// the device exists.
TEST_F(RuntimePresentationSurfaceTest, AMetalLayerSettlesItsFormatWithoutAnAdapter) {
  EXPECT_THAT(surface_.chooseConfiguration(wgpu::Adapter(), /*enableReadback=*/false),
              testing::IsTrue())
      << "a Metal layer's format is not a question for an adapter";
  EXPECT_THAT(surface_.format(), testing::Eq(gpu::TextureFormat::BGRA8Unorm));
}
#else
/// Everywhere but Apple the window settles the format its renderer compiles pipelines for by
/// asking the selected adapter what the surface can present. With no adapter there is nothing to
/// ask, so answering anyway settles a format nothing checked: the browser arm of selection reached
/// this with a null adapter and the window went on to configure the swapchain from the reply.
TEST_F(RuntimePresentationSurfaceTest, ChoosingAConfigurationWithoutAnAdapterIsRefused) {
  EXPECT_FALSE(surface_.chooseConfiguration(wgpu::Adapter(), /*enableReadback=*/false))
      << "a selection that produced no adapter has not produced a surface configuration either";
}
#endif

TEST_F(RuntimePresentationSurfaceTest, ASurfaceThatCannotPresentTheCompiledFormatIsRefused) {
  device_.formats = {gpu::TextureFormat::RGBA8Unorm};

  EXPECT_FALSE(surface_.attachToRuntime(device_, NativeHandle(), gpu::TextureFormat::BGRA8Unorm,
                                        /*enableReadback=*/false))
      << "the renderer's pipelines were compiled for a format this surface never presents";
  EXPECT_EQ(device_.createdSurfaces.size(), 1u);

  surface_.shutdown();

  EXPECT_EQ(device_.destroySurfaceCalls, 1)
      << "the surface it built to ask the question is given up with the refusal, not left behind";
}

TEST_F(RuntimePresentationSurfaceTest, AFrameThatDidNotArriveInTimeLeavesNothingHeld) {
  ASSERT_NO_FATAL_FAILURE(attachAndConfigure());
  device_.acquireStatus = gpu::SurfaceStatus::Timeout;

  internal::AcquiredFrame frame = surface_.acquire();

  EXPECT_THAT(frame.status, testing::Eq(gpu::SurfaceStatus::Timeout));
  EXPECT_FALSE(frame.texture.isValid()) << "nothing became available to draw into";

  surface_.abandon();
  surface_.present();

  EXPECT_EQ(device_.abandonCalls, 0)
      << "a frame that never came is not one to hand back, and asking would be refused";
  EXPECT_EQ(device_.presentCalls, 0);

  device_.acquireStatus = gpu::SurfaceStatus::Success;
  internal::AcquiredFrame next = surface_.acquire();
  EXPECT_TRUE(next.texture.isValid()) << "the surface is still the window's and still serves";
}

TEST_F(RuntimePresentationSurfaceTest, AnOutOfDateFrameIsStillHeldUntilTheSurfaceFollowsTheWindow) {
  ASSERT_NO_FATAL_FAILURE(attachAndConfigure());
  device_.acquireStatus = gpu::SurfaceStatus::Outdated;

  internal::AcquiredFrame frame = surface_.acquire();

  EXPECT_THAT(frame.status, testing::Eq(gpu::SurfaceStatus::Outdated));
  EXPECT_TRUE(frame.texture.isValid())
      << "a configuration that has drifted out of date usually still presents";

  device_.acquireStatus = gpu::SurfaceStatus::Success;
  EXPECT_TRUE(surface_.configure(800, 600));

  EXPECT_EQ(device_.abandonCalls, 1)
      << "the frame the platform was still holding goes back as the surface follows the window";
  EXPECT_THAT(useFrame(frame.texture), gpu::IsGpuError(gpu::GpuErrorType::InvalidHandle));
  EXPECT_TRUE(surface_.acquire().texture.isValid());
}

TEST_F(RuntimePresentationSurfaceTest, ReadbackIsDroppedWhenFramesCannotBeCopiedFrom) {
  device_.usages = gpu::TextureUsage::RenderAttachment;

  ASSERT_TRUE(surface_.attachToRuntime(device_, NativeHandle(), gpu::TextureFormat::BGRA8Unorm,
                                       /*enableReadback=*/true));
  ASSERT_TRUE(surface_.configure(640, 480));

  EXPECT_THAT(surface_.usage(), testing::Eq(gpu::TextureUsage::RenderAttachment))
      << "a frame that cannot be copied out of is still a frame worth showing";
  EXPECT_THAT(device_.lastConfiguration.usage, testing::Eq(gpu::TextureUsage::RenderAttachment));
}

TEST_F(RuntimePresentationSurfaceTest, AFrameThatCannotBeCopiedFromIsStillPresented) {
  device_.usages = gpu::TextureUsage::RenderAttachment;
  ASSERT_TRUE(surface_.attachToRuntime(device_, NativeHandle(), gpu::TextureFormat::BGRA8Unorm,
                                       /*enableReadback=*/true));
  ASSERT_TRUE(surface_.configure(640, 480));

  internal::AcquiredFrame frame = surface_.acquire();
  ASSERT_THAT(frame.status, testing::Eq(gpu::SurfaceStatus::Success));
  ASSERT_THAT(frame.texture.isValid(), testing::IsTrue())
      << "a surface whose frames cannot be copied from still hands them out to draw into";
  surface_.present();

  EXPECT_THAT(device_.presentCalls, testing::Eq(1))
      << "the frame was not shown because it could not be read back";
  EXPECT_THAT(device_.abandonCalls, testing::Eq(0));
}

TEST_F(RuntimePresentationSurfaceTest, ReadbackIsConfiguredWhenFramesCanBeCopiedFrom) {
  ASSERT_TRUE(surface_.attachToRuntime(device_, NativeHandle(), gpu::TextureFormat::BGRA8Unorm,
                                       /*enableReadback=*/true));
  ASSERT_TRUE(surface_.configure(640, 480));

  EXPECT_THAT(surface_.usage(),
              testing::Eq(gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc));
  EXPECT_THAT(device_.lastConfiguration.usage,
              testing::Eq(gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc));
}

TEST(EditorWindowTest, AlphaCompositingTakesThePreferredModeOverTheOrderOffered) {
  using gpu::SurfaceAlphaMode;

  EXPECT_THAT(
      internal::ChooseSurfaceAlphaMode({SurfaceAlphaMode::Premultiplied, SurfaceAlphaMode::Opaque},
                                       SurfaceAlphaMode::Opaque),
      testing::Eq(SurfaceAlphaMode::Opaque))
      << "the window's own compositing wins over whichever mode the surface happened to list "
         "first";
  EXPECT_THAT(
      internal::ChooseSurfaceAlphaMode({SurfaceAlphaMode::Opaque, SurfaceAlphaMode::Premultiplied},
                                       SurfaceAlphaMode::Premultiplied),
      testing::Eq(SurfaceAlphaMode::Premultiplied));

  EXPECT_THAT(internal::ChooseSurfaceAlphaMode({SurfaceAlphaMode::Inherit},
                                               SurfaceAlphaMode::Premultiplied),
              testing::Eq(SurfaceAlphaMode::Inherit))
      << "a preference the surface never offered would present a transparent clear as solid "
         "black, so an offered mode is taken instead";

  EXPECT_THAT(internal::ChooseSurfaceAlphaMode({}, SurfaceAlphaMode::Premultiplied),
              testing::Eq(SurfaceAlphaMode::Opaque))
      << "a surface that named no mode is composited opaque, which every surface does";
}

TEST_F(RuntimePresentationSurfaceTest, AlphaCompositingFollowsWhatTheSurfaceOffers) {
  device_.alphaModes = {gpu::SurfaceAlphaMode::Premultiplied};

  ASSERT_NO_FATAL_FAILURE(attachAndConfigure(Vector2i(640, 480)));

  EXPECT_TRUE(surface_.premultipliedAlpha())
      << "a surface that only composites premultiplied is reported as doing so, because the "
         "window's clear color depends on it";
  EXPECT_THAT(device_.lastConfiguration.alphaMode,
              testing::Eq(gpu::SurfaceAlphaMode::Premultiplied));
}

TEST_F(RuntimePresentationSurfaceTest, GivingUpTheSurfaceHandsBackTheFrameItHeld) {
  ASSERT_NO_FATAL_FAILURE(attachAndConfigure());
  internal::AcquiredFrame frame = surface_.acquire();
  ASSERT_TRUE(frame.texture.isValid());

  surface_.shutdown();

  EXPECT_EQ(device_.abandonCalls, 1)
      << "the platform is still holding the frame, so it is given back before the surface goes";
  EXPECT_EQ(device_.destroySurfaceCalls, 1);
  EXPECT_THAT(useFrame(frame.texture), gpu::IsGpuError(gpu::GpuErrorType::InvalidHandle));
}
#endif

TEST(EditorWindowTest, WasmSurfaceFailuresUseStatusAwareBoundedRetries) {
  using Failure = internal::WgpuSurfaceFailureKind;

  EXPECT_EQ(internal::WgpuSurfaceRetryDecisionFor(Failure::Timeout, 0u),
            (internal::WgpuSurfaceRetryDecision{.requestFrame = true, .reconfigure = false}));
  EXPECT_EQ(internal::WgpuSurfaceRetryDecisionFor(Failure::OutdatedOrLost, 1u),
            (internal::WgpuSurfaceRetryDecision{.requestFrame = true, .reconfigure = true}));
  EXPECT_EQ(internal::WgpuSurfaceRetryDecisionFor(Failure::Setup, 2u),
            (internal::WgpuSurfaceRetryDecision{.requestFrame = true, .reconfigure = false}));
  EXPECT_EQ(internal::WgpuSurfaceRetryDecisionFor(Failure::Timeout, 3u),
            (internal::WgpuSurfaceRetryDecision{}));
  EXPECT_EQ(internal::WgpuSurfaceRetryDecisionFor(Failure::Fatal, 0u),
            (internal::WgpuSurfaceRetryDecision{}));
}

TEST(EditorWindowTest, WasmSurfaceClearColorNeverRequiresUnsupportedTransparency) {
  constexpr std::array<float, 4> kTransparent = {0.11f, 0.11f, 0.13f, 0.0f};

  // A premultiplied-alpha surface composites the transparent clear correctly,
  // so uncovered render-pane pixels keep showing the worker document canvas.
  EXPECT_EQ(internal::WasmSurfaceClearColor(kTransparent, /*premultipliedAlphaSupported=*/true),
            kTransparent);

  // Without premultiplied compositing the same clear would present as opaque
  // black; fall back to the page background already painted behind the canvas.
  EXPECT_EQ(internal::WasmSurfaceClearColor(kTransparent, /*premultipliedAlphaSupported=*/false),
            internal::kWasmOpaqueSurfaceClearColor);
  EXPECT_FLOAT_EQ(internal::kWasmOpaqueSurfaceClearColor[3], 1.0f);
}

TEST(EditorWindowTest, WasmDiagnosticReadbackFailuresUseBoundedRetries) {
  EXPECT_EQ(internal::WgpuDiagnosticReadbackDecisionFor(true, 2u),
            (internal::WgpuDiagnosticReadbackDecision{
                .retry = false,
                .completeRequest = true,
            }));
  EXPECT_EQ(internal::WgpuDiagnosticReadbackDecisionFor(false, 0u),
            (internal::WgpuDiagnosticReadbackDecision{
                .retry = true,
                .completeRequest = false,
            }));
  EXPECT_EQ(internal::WgpuDiagnosticReadbackDecisionFor(false, 1u),
            (internal::WgpuDiagnosticReadbackDecision{
                .retry = true,
                .completeRequest = false,
            }));
  EXPECT_EQ(internal::WgpuDiagnosticReadbackDecisionFor(false, 2u),
            (internal::WgpuDiagnosticReadbackDecision{
                .retry = false,
                .completeRequest = true,
            }));
}

TEST(EditorWindowTest, WasmDiagnosticReadbackSetupFailuresCompleteOnThirdAttempt) {
  unsigned consecutiveFailures = 0u;
  unsigned attempts = 0u;
  bool requestCompleted = false;

  while (!requestCompleted && attempts < 10u) {
    const internal::WgpuDiagnosticReadbackDecision decision =
        internal::WgpuDiagnosticReadbackDecisionFor(/*captureSucceeded=*/false,
                                                    consecutiveFailures);
    ++attempts;
    EXPECT_TRUE(internal::ShouldRecheckPendingWgpuReadbackRequestsAfterCompletion(
        /*callbackAlive=*/true, decision));
    if (decision.completeRequest) {
      consecutiveFailures = 0u;
      requestCompleted = true;
    } else {
      ++consecutiveFailures;
    }
  }

  EXPECT_TRUE(requestCompleted);
  EXPECT_EQ(attempts, 3u);
  EXPECT_EQ(consecutiveFailures, 0u);
}

TEST(EditorWindowTest, WasmDiagnosticReadbackCompletionAlwaysRechecksPendingRequests) {
  const internal::WgpuDiagnosticReadbackDecision successfulCapture =
      internal::WgpuDiagnosticReadbackDecisionFor(true, 0u);
  const internal::WgpuDiagnosticReadbackDecision transientFailure =
      internal::WgpuDiagnosticReadbackDecisionFor(false, 0u);
  const internal::WgpuDiagnosticReadbackDecision terminalFailure =
      internal::WgpuDiagnosticReadbackDecisionFor(false, 2u);

  EXPECT_TRUE(internal::ShouldRecheckPendingWgpuReadbackRequestsAfterCompletion(
      /*callbackAlive=*/true, successfulCapture));
  EXPECT_TRUE(internal::ShouldRecheckPendingWgpuReadbackRequestsAfterCompletion(
      /*callbackAlive=*/true, transientFailure));
  EXPECT_TRUE(internal::ShouldRecheckPendingWgpuReadbackRequestsAfterCompletion(
      /*callbackAlive=*/true, terminalFailure));
  EXPECT_FALSE(internal::ShouldRecheckPendingWgpuReadbackRequestsAfterCompletion(
      /*callbackAlive=*/false, transientFailure));
}

#if defined(DONNER_EDITOR_WGPU)
TEST(EditorWindowTest, WgpuReuploadsPrebuiltFontAtlasWhenRuntimeTextureIsMissing) {
  EditorWindow window(EditorWindowOptions{
      .title = "Prebuilt Font Atlas Test",
      .initialWidth = 64,
      .initialHeight = 48,
      .visible = false,
  });
  if (!window.valid()) {
    GTEST_SKIP() << "WGPU-backed hidden editor window is unavailable on this host";
  }

  ImFontAtlas& atlas = *ImGui::GetIO().Fonts;
  ASSERT_NE(atlas.AddFontDefault(), nullptr);
  ASSERT_TRUE(atlas.Build());
  ASSERT_EQ(atlas.TexID, 0u)
      << "The regression requires a CPU-built atlas whose runtime texture was invalidated";

  window.beginFrame();
  EXPECT_NE(atlas.TexID, 0u)
      << "beginFrame must upload a built atlas when its runtime texture is missing";
  window.endFrame();
}

#if defined(__linux__)
TEST(EditorWindowTest, WgpuOffscreenTargetSupportsHeadlessReadback) {
  EditorWindow window(EditorWindowOptions{
      .title = "Headless WGPU Readback Test",
      .initialWidth = 64,
      .initialHeight = 48,
      .visible = false,
      .offscreen = true,
      .clearColor = {0.0f, 0.0f, 1.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  ASSERT_TRUE(window.valid());
  ASSERT_NE(window.geodeFramebufferDevice(), nullptr);

  window.beginFrame();
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();
  ASSERT_FALSE(actual.empty());
  EXPECT_EQ(actual.dimensions, Vector2i(64, 48));
  EXPECT_THAT(PixelAt(actual, 8, 8),
              Rgba(testing::Le(3), testing::Le(3), Near(255, 3), testing::Eq(255)));
  EXPECT_TRUE(window.usingOffscreenRenderTarget());
}

TEST(EditorWindowTest, OffscreenWindowDoesNotPinLaterWindowToNullPlatform) {
  if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr) {
    GTEST_SKIP() << "A display is needed to distinguish a window surface from the null platform";
  }

  glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
  {
    EditorWindow before(EditorWindowOptions{
        .title = "Display Window Before Offscreen",
        .initialWidth = 64,
        .initialHeight = 48,
        .visible = false,
    });
    ASSERT_THAT(before.valid(), testing::IsTrue())
        << "A declared display must open a window for this regression to be meaningful";
    EXPECT_THAT(glfwGetPlatform(), testing::Ne(GLFW_PLATFORM_NULL));
  }

  {
    EditorWindow offscreen(EditorWindowOptions{
        .title = "Offscreen Window Between Display Windows",
        .initialWidth = 64,
        .initialHeight = 48,
        .visible = false,
        .offscreen = true,
    });
    ASSERT_THAT(offscreen.valid(), testing::IsTrue());
    EXPECT_THAT(glfwGetPlatform(), testing::Eq(GLFW_PLATFORM_NULL));
  }

  EditorWindow after(EditorWindowOptions{
      .title = "Display Window After Offscreen",
      .initialWidth = 64,
      .initialHeight = 48,
      .visible = false,
  });
  ASSERT_THAT(after.valid(), testing::IsTrue())
      << "An offscreen window must not leave the next window on GLFW's null platform";
  EXPECT_THAT(glfwGetPlatform(), testing::Ne(GLFW_PLATFORM_NULL));
}

TEST(EditorWindowTest, NativeVulkanWindowsRetainGlfwUntilTheLastWindowCloses) {
  if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr) {
    GTEST_SKIP() << "A display is required for native Vulkan window presentation";
  }
  const gpu::Result<geode::GpuBackendKind> selected = geode::ProcessDefaultGpuBackendKind();
  ASSERT_THAT(selected, gpu::HasResult());
  if (selected.result() != geode::GpuBackendKind::NativeVulkan) {
    GTEST_SKIP() << "This run did not select native Vulkan";
  }
  const uint64_t terminations = internal::GlfwTerminationCountForTesting();
  glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
  ASSERT_TRUE(internal::AcquireGlfwRuntimeForTesting());
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow* survivor = glfwCreateWindow(64, 48, "Companion GLFW Window", nullptr, nullptr);
  if (survivor == nullptr) {
    internal::ReleaseGlfwRuntimeForTesting();
    FAIL() << "the companion GLFW window could not open";
  }
  {
    EditorWindow editor(EditorWindowOptions{.title = "Vulkan Editor Window",
                                            .initialWidth = 64,
                                            .initialHeight = 48,
                                            .visible = false});
    EXPECT_TRUE(editor.valid());
    if (editor.valid()) {
      editor.beginFrame();
      editor.endFrame();
    }
  }
  EXPECT_EQ(internal::GlfwTerminationCountForTesting(), terminations);
  int width = 0;
  int height = 0;
  glfwGetWindowSize(survivor, &width, &height);
  EXPECT_GT(width, 0);
  EXPECT_GT(height, 0);
  glfwDestroyWindow(survivor);
  internal::ReleaseGlfwRuntimeForTesting();
  EXPECT_EQ(internal::GlfwTerminationCountForTesting(), terminations + 1);
  internal::ReleaseGlfwRuntimeForTesting();
  EXPECT_EQ(internal::GlfwTerminationCountForTesting(), terminations + 1);
}

TEST(EditorWindowTest, LostNativeVulkanSurfaceStopsLaterFrameRetries) {
  if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr) {
    GTEST_SKIP() << "A display is required for native Vulkan window presentation";
  }
  const gpu::Result<geode::GpuBackendKind> selected = geode::ProcessDefaultGpuBackendKind();
  ASSERT_THAT(selected, gpu::HasResult());
  if (selected.result() != geode::GpuBackendKind::NativeVulkan) {
    GTEST_SKIP() << "This run did not select native Vulkan";
  }
  EditorWindow window(EditorWindowOptions{
      .title = "Lost Vulkan Editor Surface",
      .initialWidth = 64,
      .initialHeight = 48,
      .visible = false,
  });
  ASSERT_TRUE(window.valid());
  int drawnFrames = 0;
  window.setWgpuUnderlayRenderCallback([&](const EditorWindowWgpuRenderTarget&) { ++drawnFrames; });
  window.beginFrame();
  window.endFrame();
  ASSERT_EQ(drawnFrames, 1) << "the native surface must first draw a real frame";

  testing::internal::CaptureStderr();
  window.forcePresentationSurfaceLossForTesting();
  for (int frame = 0; frame < 3; ++frame) {
    window.beginFrame();
    window.endFrame();
  }
  const std::string errors = testing::internal::GetCapturedStderr();
  constexpr std::string_view kFailedRebuild = "could not be rebuilt from the window";
  std::size_t rebuildReports = 0;
  for (std::size_t pos = errors.find(kFailedRebuild); pos != std::string::npos;
       pos = errors.find(kFailedRebuild, pos + kFailedRebuild.size())) {
    ++rebuildReports;
  }
  EXPECT_EQ(rebuildReports, 1u) << errors;
  EXPECT_EQ(drawnFrames, 1) << "the lost native surface cannot draw another frame";
  EXPECT_FALSE(window.geodeFramebufferDevice()->isDeviceLost())
      << "only the platform surface was lost; the shared device remains healthy";
  EXPECT_FALSE(window.framebufferReadbackAvailable());
}

TEST(EditorWindowDeathTest, UnprovenNativeRetirementQuarantinesTheWindowAndGlfwClaim) {
  if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr) {
    GTEST_SKIP() << "A display is required for native Vulkan window presentation";
  }
  const gpu::Result<geode::GpuBackendKind> selected = geode::ProcessDefaultGpuBackendKind();
  ASSERT_THAT(selected, gpu::HasResult());
  if (selected.result() != geode::GpuBackendKind::NativeVulkan) {
    GTEST_SKIP() << "This run did not select native Vulkan";
  }
  // Coverage instrumentation may start threads before any test runs; re-exec the death-test child.
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  ASSERT_EXIT(([&] {
                auto window = std::make_unique<EditorWindow>(EditorWindowOptions{
                    .title = "Retained Vulkan Window",
                    .initialWidth = 64,
                    .initialHeight = 48,
                    .visible = false,
                });
                ASSERT_TRUE(window->valid());
                auto& native = static_cast<gpu::vulkan::VulkanDevice&>(
                    window->geodeFramebufferDevice()->runtimeDevice());
                native.forceSurfaceRetirementUnprovenForTest();
                window.reset();
                EXPECT_NE(glfwGetPlatform(), GLFW_PLATFORM_NULL);
                EditorWindow incompatible(EditorWindowOptions{
                    .title = "Null Platform After Quarantine",
                    .initialWidth = 32,
                    .initialHeight = 32,
                    .visible = false,
                    .offscreen = true,
                });
                EXPECT_FALSE(incompatible.valid());
                std::exit(testing::Test::HasFailure() ? 1 : 0);
              }()),
              testing::ExitedWithCode(0), "");
}
#endif

TEST(EditorWindowTest, NumericDragFieldsSupportSimpleClickToEdit) {
  EditorWindow window(EditorWindowOptions{
      .title = "Numeric Click To Edit Test",
      .initialWidth = 64,
      .initialHeight = 64,
      .visible = false,
  });
  if (!window.valid()) {
    GTEST_SKIP() << "Editor window is unavailable on this host";
  }

  EXPECT_TRUE(ImGui::GetIO().ConfigDragClickToInputText);
}

/// Opens a window presenting to its surface (false) or rendering offscreen (true).
class EditorWindowBackendTest : public testing::TestWithParam<bool> {};

/// The window renders through whichever backend the process selects, on the arm that presents
/// to a window surface and the arm that renders offscreen alike, and its surface, UI renderer and
/// UI texture registry come up on that backend's device.
TEST_P(EditorWindowBackendTest, OpensOnTheBackendTheProcessSelected) {
  const gpu::Result<geode::GpuBackendKind> selected = geode::ProcessDefaultGpuBackendKind();
  ASSERT_THAT(selected, gpu::HasResult());
  SCOPED_TRACE(testing::Message() << "selected backend: " << selected.result());

  EditorWindow window(EditorWindowOptions{
      .title = "Selected Backend Window Test",
      .initialWidth = 64,
      .initialHeight = 48,
      .visible = false,
      .forceOffscreenRenderTarget = GetParam(),
  });
  ASSERT_THAT(window.valid(), testing::IsTrue())
      << "the window did not open on the backend the process selected";
  ASSERT_THAT(window.geodeFramebufferDevice(), testing::NotNull());
#ifdef __APPLE__
  // A hidden Cocoa window still presents to its Metal layer; only a forced target is offscreen.
  EXPECT_THAT(window.usingOffscreenRenderTarget(), testing::Eq(GetParam()));
#else
  // A host without a display renders every window offscreen, so only the forced arm is known.
  if (GetParam()) {
    EXPECT_THAT(window.usingOffscreenRenderTarget(), testing::IsTrue());
  }
#endif
  EXPECT_THAT(window.geodeFramebufferDevice()->physicalDeviceOwner()->root().capabilities().backend,
              testing::Eq(selected.result()));
  ASSERT_THAT(CurrentImGuiRuntimeRenderer(), testing::NotNull());
  EXPECT_THAT(CurrentImGuiRuntimeRenderer()->device().deviceId(),
              testing::Eq(window.geodeFramebufferDevice()->runtimeDevice().deviceId()))
      << "the UI is drawn on a device other than the window's framebuffer device";
#if defined(__linux__)
  if (!GetParam() &&
      (std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr) &&
      selected.result() == geode::GpuBackendKind::NativeVulkan) {
    auto& native =
        static_cast<gpu::vulkan::VulkanDevice&>(window.geodeFramebufferDevice()->runtimeDevice());
    EXPECT_TRUE(native.supportsPresentation());
    EXPECT_NE(native.nativeInstance(), nullptr);
    EXPECT_FALSE(window.usingOffscreenRenderTarget());
    window.beginFrame();
    window.endFrame();
    glfwSetWindowSize(window.rawHandle(), 80, 64);
    window.pollEvents();
    window.beginFrame();
    window.endFrame();
  }
#endif
}

INSTANTIATE_TEST_SUITE_P(Targets, EditorWindowBackendTest, testing::Bool(),
                         [](const testing::TestParamInfo<bool>& info) {
                           return info.param ? std::string("Offscreen")
                                             : std::string("WindowSurface");
                         });

/// Which of a window's configurations a lifecycle case runs on.
struct LifecycleConfiguration {
  /// Whether the window renders into its own offscreen target rather than presenting to a
  /// surface.
  bool offscreen = false;
  /// Whether the frames the window draws into can be copied from, so reading one back returns
  /// it. A surface that reports its frames cannot be copied from gives a window with readback
  /// asked for the same frames as a window that never asked, which is how a case reaches it on
  /// any host.
  bool framesCopyable = true;
};

/// Prints \p configuration by the name its cases carry. @param configuration Value to print.
/// @param os Stream to print to.
void PrintTo(const LifecycleConfiguration& configuration, std::ostream* os) {
  *os << (configuration.offscreen ? "Offscreen" : "WindowSurface")
      << (configuration.framesCopyable ? "" : "WithoutCopyableFrames");
}

/// A real window, presenting to its surface or rendering offscreen, with frames it can or cannot
/// copy back, taken through what happens to a window during its life: being resized, and losing
/// its device. Only Apple always has a surface to present to; a host without a display renders
/// both arms offscreen.
class EditorWindowLifecycleTest : public testing::TestWithParam<LifecycleConfiguration> {
protected:
  /// A hidden window on the parameter's arm that reads its frames back and clears to opaque blue.
  /// Its width leaves the readback's rows unaligned to the copy row pitch at 1x and at 2x, so
  /// reading a frame back always repacks them.
  EditorWindowOptions options() const {
    return EditorWindowOptions{
        .title = "Window Lifecycle Test",
        .initialWidth = 100,
        .initialHeight = 48,
        .visible = false,
        .forceOffscreenRenderTarget = GetParam().offscreen,
        .clearColor = {0.0f, 0.0f, 1.0f, 1.0f},
        .enableFramebufferReadback = GetParam().framesCopyable,
    };
  }

  /**
   * Whether \p window's frames read back, which is what decides whether a case checks their
   * texels. A window whose frames were configured without copy usage reports so; a window that
   * asked for readback may still not get it, where its surface reports frames that cannot be
   * copied from, and a case there checks everything but the texels.
   *
   * @param window Window under test.
   */
  bool framesReadBack(const EditorWindow& window) const {
    const bool available = window.framebufferReadbackAvailable();
    if (!GetParam().framesCopyable) {
      EXPECT_THAT(available, testing::IsFalse())
          << "a window whose frames cannot be copied from reports that they read back";
      return available;
    }
    // A window's own offscreen target always carries copy usage when readback is asked for, and
    // so does a Metal layer's frame; only another platform's surface can refuse it.
    bool guaranteed = window.usingOffscreenRenderTarget();
#ifdef __APPLE__
    guaranteed = true;
#endif
    if (guaranteed) {
      EXPECT_THAT(available, testing::IsTrue())
          << "a window whose frames can be copied from reports that they do not read back";
    } else if (!available) {
      std::cout << "[  NOTE    ] frames are not read back on this host: its presentation surface "
                   "reports that they cannot be copied from"
                << std::endl;
    }
    return available;
  }

  /// The extents one drawn frame carried: what the frame loop reported to the callback, and the
  /// texture the frame was actually drawn into.
  struct DrawnFrame {
    Vector2i reported;  //!< Extent the frame loop reported.
    Vector2i target;    //!< Extent of the frame's target texture on the framebuffer device.

    /// Equality operator. @param other Frame to compare against.
    bool operator==(const DrawnFrame& other) const = default;

    /// Ostream output operator. @param os Output stream. @param frame Frame to output.
    friend std::ostream& operator<<(std::ostream& os, const DrawnFrame& frame) {
      return os << "{reported=" << frame.reported << ", target=" << frame.target << "}";
    }
  };

  /// A frame drawn at \p extent, as reported and as allocated. @param extent Framebuffer extent.
  static DrawnFrame DrawnAt(Vector2i extent) { return DrawnFrame{extent, extent}; }

  /// Records each frame the window draws, through a callback the window hands every frame's target
  /// before its UI: the extent it reported and the extent of the texture it drew into, which a
  /// target that was never reallocated or reconfigured would not share.
  /// @param window Window to watch. @param frames Receives them.
  static void recordDrawnFrames(EditorWindow& window, std::vector<DrawnFrame>* frames) {
    gpu::Device& device = window.geodeFramebufferDevice()->runtimeDevice();
    window.setWgpuUnderlayRenderCallback([frames,
                                          &device](const EditorWindowWgpuRenderTarget& target) {
      const gpu::Result<gpu::TextureDescriptor> described =
          device.textureDescriptor(target.texture);
      const Vector2i targetExtent = described.hasResult()
                                        ? Vector2i(static_cast<int>(described.result().size.width),
                                                   static_cast<int>(described.result().size.height))
                                        : Vector2i::Zero();
      frames->push_back(DrawnFrame{target.framebufferSizePx, targetExtent});
    });
  }

  /// Checks \p window is on the arm the parameter asked for, where that is known: a hidden Cocoa
  /// window still presents to its Metal layer, so on Apple only the forced arm is offscreen.
  /// @param window Window under test.
  void expectOnTheRequestedArm(const EditorWindow& window) const {
#ifdef __APPLE__
    ASSERT_THAT(window.usingOffscreenRenderTarget(), testing::Eq(GetParam().offscreen))
        << "the window is not on the arm this case is about";
#else
    if (GetParam().offscreen) {
      ASSERT_THAT(window.usingOffscreenRenderTarget(), testing::IsTrue());
    }
#endif
  }
};

/// A resized window follows its new framebuffer extent on the next frame: a presented window
/// reconfigures its surface, an offscreen one reallocates its target, and the frame is drawn at
/// the new extent. Where its frames read back, the frame reads back at that extent, including the
/// texels the old extent did not cover.
TEST_P(EditorWindowLifecycleTest, AResizedWindowDrawsAtItsNewExtent) {
  EditorWindow window(options());
  ASSERT_THAT(window.valid(), testing::IsTrue());
  ASSERT_NO_FATAL_FAILURE(expectOnTheRequestedArm(window));
  const bool readsBack = framesReadBack(window);
  std::vector<DrawnFrame> drawnFrames;
  recordDrawnFrames(window, &drawnFrames);

  const Vector2i initial = window.framebufferSize();
  window.beginFrame();
  const svg::RendererBitmap before = window.endFrameAndReadPixels();
  ASSERT_THAT(drawnFrames, testing::ElementsAre(DrawnAt(initial)))
      << "the first frame was not drawn at the window's extent";
  if (readsBack) {
    ASSERT_THAT(before.empty(), testing::IsFalse());
    ASSERT_THAT(before.dimensions, testing::Eq(initial));
  }

  glfwSetWindowSize(window.rawHandle(), 150, 72);
  window.pollEvents();
  const Vector2i resized = window.framebufferSize();
  ASSERT_THAT(resized, testing::Ne(initial)) << "the window was not resized";

  window.beginFrame();
  const svg::RendererBitmap after = window.endFrameAndReadPixels();
  EXPECT_THAT(drawnFrames, testing::ElementsAre(DrawnAt(initial), DrawnAt(resized)))
      << "the resized window's frame was not drawn into a target of its new extent";
  if (!readsBack) {
    EXPECT_THAT(after.empty(), testing::IsTrue()) << "a frame that cannot be copied was read";
    return;
  }
  ASSERT_THAT(after.empty(), testing::IsFalse()) << "the resized window's frame read back empty";
  EXPECT_THAT(after.dimensions, testing::Eq(resized));
  EXPECT_THAT(after.rowBytes, testing::Eq(static_cast<std::size_t>(resized.x) * 4u));
  EXPECT_THAT(PixelAt(after, resized.x - 1, resized.y - 1),
              Rgba(testing::Le(3), testing::Le(3), Near(255, 3), testing::Eq(255)))
      << "a texel outside the old extent was not drawn at the new one";
}

/// A device declared lost stops the window before another surface acquisition or draw, and no
/// frame after it spends the readback bound. The loss reaches the window through the condition
/// its framebuffer context shares with the runtime device on either backend.
TEST_P(EditorWindowLifecycleTest, FramesAfterADeclaredLossReadBackNothingWithinTheBound) {
  EditorWindow window(options());
  ASSERT_THAT(window.valid(), testing::IsTrue());
  ASSERT_NO_FATAL_FAILURE(expectOnTheRequestedArm(window));
  const bool readsBack = framesReadBack(window);
  std::vector<DrawnFrame> drawnFrames;
  recordDrawnFrames(window, &drawnFrames);

  window.beginFrame();
  const svg::RendererBitmap beforeLoss = window.endFrameAndReadPixels();
  ASSERT_THAT(drawnFrames, testing::ElementsAre(DrawnAt(window.framebufferSize())))
      << "the frame before the loss was not drawn";
  if (readsBack) {
    ASSERT_THAT(beforeLoss.empty(), testing::IsFalse()) << "the frame before the loss read nothing";
  }

  window.geodeFramebufferDevice()->markDeviceLost("declared lost by the window lifecycle test");
  EXPECT_THAT(window.geodeFramebufferDevice()->runtimeDevice().isLost(), testing::IsTrue())
      << "the framebuffer context and its runtime device do not share one loss condition";

  for (int frame = 0; frame < 2; ++frame) {
    SCOPED_TRACE(testing::Message() << "frame " << frame << " after the loss");
    const auto frameStart = std::chrono::steady_clock::now();
    window.beginFrame();
    const svg::RendererBitmap lost = window.endFrameAndReadPixels();
    const auto frameTime = std::chrono::steady_clock::now() - frameStart;
    EXPECT_THAT(lost.empty(), testing::IsTrue()) << "a lost device's frame was read back";
    EXPECT_THAT(drawnFrames, testing::ElementsAre(DrawnAt(window.framebufferSize())))
        << "a known-lost device must not attempt another frame";
    EXPECT_THAT(window.framebufferReadbackAvailable(), testing::IsFalse());
    EXPECT_THAT(std::chrono::duration_cast<std::chrono::milliseconds>(frameTime),
                testing::Lt(geode::kDefaultGpuWaitTimeout))
        << "the frame waited out the readback bound on a lost device";
  }
}

INSTANTIATE_TEST_SUITE_P(
    Targets, EditorWindowLifecycleTest,
    testing::Values(LifecycleConfiguration{.offscreen = false, .framesCopyable = true},
                    LifecycleConfiguration{.offscreen = true, .framesCopyable = true},
                    LifecycleConfiguration{.offscreen = false, .framesCopyable = false},
                    LifecycleConfiguration{.offscreen = true, .framesCopyable = false}),
    [](const testing::TestParamInfo<LifecycleConfiguration>& info) {
      return testing::PrintToString(info.param);
    });

TEST(EditorWindowTest, WgpuFramebufferGeodeDeviceSharingMatchesThreadingModel) {
  EditorWindow window(EditorWindowOptions{
      .title = "Shared WGPU Geode Device Test",
      .initialWidth = 64,
      .initialHeight = 64,
      .visible = false,
  });
  if (!window.valid() || window.geodeDevice() == nullptr ||
      window.geodeFramebufferDevice() == nullptr) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }

#ifdef __EMSCRIPTEN__
  EXPECT_NE(window.geodeFramebufferDevice().get(), window.geodeDevice().get());
  EXPECT_EQ(window.geodeFramebufferDevice()->physicalDeviceOwner(),
            window.geodeDevice()->physicalDeviceOwner());
  EXPECT_NE(window.geodeFramebufferDevice()->deviceId(), window.geodeDevice()->deviceId());
#else
  EXPECT_NE(window.geodeFramebufferDevice().get(), window.geodeDevice().get())
      << "Desktop background rendering shares the primary wrapper across threads. The UI-only "
         "framebuffer path needs a separate wrapper to isolate mutable counters and deferred "
         "destroy queues.";
  EXPECT_EQ(window.geodeFramebufferDevice()->physicalDeviceOwner(),
            window.geodeDevice()->physicalDeviceOwner());
  EXPECT_NE(window.geodeFramebufferDevice()->deviceId(), window.geodeDevice()->deviceId());

  // Both wrappers drive one backend device. A texture of one registers on the other only then:
  // the transitional adapter also requires the two to submit to one queue, and Metal requires the
  // texture's device to be the consumer's own.
  gpu::Device& producer = window.geodeDevice()->runtimeDevice();
  gpu::Device& consumer = window.geodeFramebufferDevice()->runtimeDevice();
  gpu::Result<gpu::Texture> texture = producer.createTexture(gpu::TextureDescriptor{
      "SharedBackendDeviceProbe", gpu::Extent2d{4, 4}, gpu::TextureFormat::RGBA8Unorm,
      gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  ASSERT_THAT(texture, gpu::HasResult());
  gpu::Result<gpu::TextureExport> exported = producer.exportTexture(texture.result());
  ASSERT_THAT(exported, gpu::HasResult());
  EXPECT_THAT(consumer.registerTexture(exported.result()), gpu::HasResult())
      << "the UI framebuffer wrapper does not drive the backend device the renderer does";
#endif
}

#ifndef __EMSCRIPTEN__
TEST(EditorWindowTest, WgpuPhysicalDeviceOutlivesWindowWhenContextIsRetained) {
  std::shared_ptr<geode::GeodeDevice> retainedContext;
  std::weak_ptr<geode::GeodePhysicalDeviceOwner> physicalOwner;
  {
    EditorWindow window(EditorWindowOptions{
        .title = "Retained WGPU Context Test",
        .initialWidth = 64,
        .initialHeight = 64,
        .visible = false,
    });
    if (!window.valid() || window.geodeFramebufferDevice() == nullptr) {
      GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
    }
    retainedContext = window.geodeFramebufferDevice();
    physicalOwner = retainedContext->physicalDeviceOwner();
  }

  ASSERT_FALSE(physicalOwner.expired());
  {
    gpu::Result<gpu::Buffer> buffer = retainedContext->runtimeDevice().createBuffer(
        gpu::BufferDescriptor{"RetainedContextBuffer", 16, gpu::BufferUsage::CopyDst});
    EXPECT_THAT(buffer, gpu::HasResult())
        << "the retained context's backend device went away with the window";
  }

  retainedContext.reset();
  EXPECT_TRUE(physicalOwner.expired());
}
#endif

/// A framebuffer readback whose map outlasts the editor's bound declares the framebuffer device
/// lost at the readback-map wait site, so later frames fail at once instead of stalling, and the
/// frame reads back nothing. The map is held pending through the transitional adapter's event-wait
/// seam, under which every wait slice reports that it waited and learned nothing.
TEST(EditorWindowTest, AReadbackMapThatOutlastsItsBoundDeclaresTheDeviceLost) {
  EditorWindow window(EditorWindowOptions{
      .title = "Readback Map Bound Test",
      .initialWidth = 64,
      .initialHeight = 48,
      .visible = false,
      .forceOffscreenRenderTarget = true,
      .enableFramebufferReadback = true,
  });
  ASSERT_THAT(window.valid(), testing::IsTrue());
  const std::shared_ptr<geode::GeodeDevice> framebufferDevice = window.geodeFramebufferDevice();
  ASSERT_THAT(framebufferDevice, testing::NotNull());
  if (!framebufferDevice->hasTransitionalAdapter()) {
    GTEST_SKIP() << "holds the readback map pending through the transitional adapter's event-wait "
                    "seam, which no native backend has";
  }
  framebufferDevice->adapterDevice().setSimulateEventWaitForTest(true);
  window.setFramebufferReadbackBudgetForTesting(std::chrono::milliseconds(50));

  window.beginFrame();
  const svg::RendererBitmap bitmap = window.endFrameAndReadPixels();

  EXPECT_THAT(bitmap.empty(), testing::IsTrue()) << "a frame whose map never completed was read";
  EXPECT_THAT(framebufferDevice->isDeviceLost(), testing::IsTrue())
      << "a map that outlasted the bound left the device answering";
  EXPECT_THAT(framebufferDevice->consumeReadbackStats().timedOutWaitSite,
              testing::Eq(geode::GpuWaitSite::ReadbackMap))
      << "the loss was not attributed to the readback map's wait";
}

TEST(EditorWindowTest, WgpuCheckerboardRejectsAStaleFramebufferExtent) {
  EditorWindow window(EditorWindowOptions{
      .title = "Stale WGPU Framebuffer Extent Test",
      .initialWidth = 96,
      .initialHeight = 96,
      .visible = false,
  });
  if (!window.valid() || window.geodeFramebufferDevice() == nullptr) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }

  FramebufferCheckerboardRenderer checkerboard(window.geodeFramebufferDevice());
  int callbackCount = 0;
  window.setWgpuUnderlayRenderCallback(
      [&checkerboard, &callbackCount](const EditorWindowWgpuRenderTarget& target) {
        ++callbackCount;
        if (callbackCount == 1) {
          EXPECT_EQ(checkerboard.draw(target,
                                      Box2d(Vector2d::Zero(), Vector2d(target.framebufferSizePx.x,
                                                                       target.framebufferSizePx.y)),
                                      target.framebufferFromLogicalScale),
                    1)
              << "The live frame extent must exercise a successful checkerboard draw before the "
                 "stale-size refusal is tested";
          return;
        }
        EditorWindowWgpuRenderTarget stale = target;
        ASSERT_GT(stale.framebufferSizePx.x, 1);
        --stale.framebufferSizePx.x;
        EXPECT_EQ(checkerboard.draw(stale,
                                    Box2d(Vector2d::Zero(), Vector2d(stale.framebufferSizePx.x,
                                                                     stale.framebufferSizePx.y)),
                                    stale.framebufferFromLogicalScale),
                  0)
            << "A resize-stale frame extent must be refused before the raw target is imported";
      });

  window.beginFrame();
  window.endFrame();
  window.beginFrame();
  window.endFrame();
  EXPECT_EQ(callbackCount, 2);
}

TEST(EditorWindowTest, WgpuDirectRenderCallbackDrawsBelowImGuiChrome) {
  EditorWindow window(EditorWindowOptions{
      .title = "Direct WGPU Framebuffer Append Test",
      .initialWidth = 96,
      .initialHeight = 96,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  if (!window.valid() || window.geodeFramebufferDevice() == nullptr) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }

  svg::RendererGeode directRenderer(window.geodeFramebufferDevice());
  window.setWgpuDirectRenderCallback([&directRenderer](const EditorWindowWgpuRenderTarget& target) {
    if (!target.texture.isValid()) {
      return;
    }

    svg::RenderViewport viewport;
    viewport.size = Vector2d(static_cast<double>(target.framebufferSizePx.x),
                             static_cast<double>(target.framebufferSizePx.y));
    viewport.devicePixelRatio = 1.0;

    const Vector2d framebufferFromLogical(static_cast<double>(target.framebufferSizePx.x) / 96.0,
                                          static_cast<double>(target.framebufferSizePx.y) / 96.0);
    directRenderer.setTargetTexture(target.texture);
    directRenderer.setPreserveTargetOnBeginFrame(true);
    directRenderer.beginFrame(viewport);

    svg::PaintParams paint;
    paint.fill = svg::PaintServer::Solid{css::Color(css::RGBA(255, 0, 0, 255))};
    paint.stroke = svg::PaintServer::None{};
    paint.opacity = 1.0;
    paint.fillOpacity = 1.0;
    directRenderer.setPaint(paint);
    directRenderer.drawRect(Box2d(Vector2d(0.0, 0.0) * framebufferFromLogical,
                                  Vector2d(96.0, 96.0) * framebufferFromLogical),
                            svg::StrokeParams{});
    directRenderer.endFrame();
    directRenderer.clearTargetTexture();
  });

  window.beginFrame();
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(32.0f, 32.0f), ImVec2(64.0f, 64.0f),
                                                IM_COL32(0, 0, 255, 255));
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();
  ASSERT_FALSE(actual.empty());
  const Vector2d readbackFromLogical = ReadbackScale(actual, 96, 96);

  const std::array<std::uint8_t, 4> outside = PixelAtLogical(actual, readbackFromLogical, 16, 16);
  EXPECT_THAT(outside, Rgba(Near(255, 3), testing::Le(3), testing::Le(3), testing::Eq(255)))
      << "The direct Geode pass must remain visible outside ImGui chrome.";

  const std::array<std::uint8_t, 4> inside = PixelAtLogical(actual, readbackFromLogical, 48, 48);
  EXPECT_THAT(inside, Rgba(testing::Le(3), testing::Le(3), Near(255, 3), testing::Eq(255)))
      << "ImGui popups and controls must paint above the direct selection overlay.";
}

TEST(EditorWindowTest, WgpuUnderlayDirectRenderCallbackDrawsBelowImGui) {
  EditorWindow window(EditorWindowOptions{
      .title = "Direct WGPU Framebuffer Underlay Test",
      .initialWidth = 96,
      .initialHeight = 96,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  if (!window.valid() || window.geodeFramebufferDevice() == nullptr) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }

  std::shared_ptr<const svg::RendererTextureSnapshot> texture =
      RenderPremultipliedRedTexture(window.geodeDevice());
  ASSERT_TRUE(texture != nullptr);
  svg::RendererGeode underlayRenderer(window.geodeFramebufferDevice());
  window.setWgpuUnderlayRenderCallback(
      [&underlayRenderer, texture](const EditorWindowWgpuRenderTarget& target) {
        if (!target.texture.isValid()) {
          return;
        }

        svg::RenderViewport viewport;
        viewport.size = Vector2d(static_cast<double>(target.framebufferSizePx.x),
                                 static_cast<double>(target.framebufferSizePx.y));
        viewport.devicePixelRatio = 1.0;

        underlayRenderer.setTargetTexture(target.texture);
        underlayRenderer.setPreserveTargetOnBeginFrame(true);
        underlayRenderer.beginFrame(viewport);

        ASSERT_TRUE(underlayRenderer.drawTextureSnapshot(
            *texture,
            Box2d(Vector2d::Zero(), Vector2d(static_cast<double>(target.framebufferSizePx.x),
                                             static_cast<double>(target.framebufferSizePx.y)))));
        underlayRenderer.endFrame();
        underlayRenderer.clearTargetTexture();
      });

  window.beginFrame();
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(32.0f, 32.0f), ImVec2(64.0f, 64.0f),
                                                IM_COL32(0, 0, 255, 255));
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();
  ASSERT_FALSE(actual.empty());
  const Vector2d readbackFromLogical = ReadbackScale(actual, 96, 96);

  const std::array<std::uint8_t, 4> underlayOnly =
      PixelAtLogical(actual, readbackFromLogical, 16, 16);
  EXPECT_THAT(underlayOnly, Rgba(Near(128, 3), testing::Le(3), testing::Le(3), testing::Eq(255)))
      << "The underlay direct pass must survive ImGui rendering.";

  const std::array<std::uint8_t, 4> imguiOverUnderlay =
      PixelAtLogical(actual, readbackFromLogical, 48, 48);
  EXPECT_THAT(imguiOverUnderlay,
              Rgba(testing::Le(3), testing::Le(3), Near(255, 3), testing::Eq(255)))
      << "ImGui widgets must render above the direct document underlay.";
}

TEST(EditorWindowTest, WgpuPresentsFilledPromotedLayerAfterStyleMutation) {
  EditorWindow window(EditorWindowOptions{
      .title = "Filled Promoted Layer WGPU Presentation Test",
      .initialWidth = 160,
      .initialHeight = 160,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  if (!window.valid() || window.geodeDevice() == nullptr ||
      window.geodeFramebufferDevice() == nullptr) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
      <path id="target" d="M 8 8 L 56 8 L 8 56 Z"
            style="fill: none; stroke: black; stroke-width: 1"/>
    </svg>
  )svg"));
  app.document().document().setCanvasSize(64, 64);

  std::optional<svg::SVGElement> target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);
  const Entity targetEntity = target->unsafeEntityHandle().entity();

  svg::Renderer renderer(window.geodeDevice());
  if (!renderer.requiresTextureSnapshotPresentation()) {
    GTEST_SKIP() << "This regression is specific to Geode texture-snapshot presentation.";
  }

  AsyncRenderer asyncRenderer;
  const std::optional<RenderResult> before = RenderSelectedPromotedPreview(
      asyncRenderer, renderer, app, targetEntity, /*version=*/1, /*forceLayerRasterization=*/false);
  ASSERT_TRUE(before.has_value());
  ASSERT_TRUE(before->compositedPreview.has_value());
  const RenderResult::CompositedTile* beforeTile =
      FindLayerTile(*before->compositedPreview, targetEntity);
  ASSERT_NE(beforeTile, nullptr);
  ASSERT_NE(beforeTile->textureSnapshot, nullptr);

  ASSERT_TRUE(app.setStylePropertyOnSelection("fill", "#36c317"));
  ASSERT_TRUE(app.flushFrame());
  const auto cacheInvalidatedIt =
      std::ranges::find(app.document().lastFlushResult().cacheInvalidatedElements, targetEntity);
  EXPECT_NE(cacheInvalidatedIt, app.document().lastFlushResult().cacheInvalidatedElements.end());

  const std::optional<RenderResult> after = RenderSelectedPromotedPreview(
      asyncRenderer, renderer, app, targetEntity, /*version=*/2, /*forceLayerRasterization=*/true);
  ASSERT_TRUE(after.has_value());
  ASSERT_TRUE(after->compositedPreview.has_value());
  const RenderResult::CompositedTile* afterTile =
      FindLayerTile(*after->compositedPreview, targetEntity);
  ASSERT_NE(afterTile, nullptr);
  ASSERT_NE(afterTile->textureSnapshot, nullptr);
  ASSERT_GT(afterTile->generation, beforeTile->generation);

  std::shared_ptr<const svg::RendererTextureSnapshot> texture = afterTile->textureSnapshot;
  svg::RendererGeode underlayRenderer(window.geodeFramebufferDevice());
  window.setWgpuUnderlayRenderCallback(
      [&underlayRenderer, texture](const EditorWindowWgpuRenderTarget& renderTarget) {
        if (!renderTarget.texture.isValid()) {
          return;
        }

        svg::RenderViewport viewport;
        viewport.size = Vector2d(static_cast<double>(renderTarget.framebufferSizePx.x),
                                 static_cast<double>(renderTarget.framebufferSizePx.y));
        viewport.devicePixelRatio = 1.0;

        underlayRenderer.setTargetTexture(renderTarget.texture);
        underlayRenderer.setPreserveTargetOnBeginFrame(true);
        underlayRenderer.beginFrame(viewport);
        ASSERT_TRUE(underlayRenderer.drawTextureSnapshot(
            *texture, Box2d(Vector2d(16.0, 16.0),
                            Vector2d(static_cast<double>(renderTarget.framebufferSizePx.x - 16),
                                     static_cast<double>(renderTarget.framebufferSizePx.y - 16)))));
        underlayRenderer.endFrame();
        underlayRenderer.clearTargetTexture();
      });

  window.beginFrame();
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();
  WriteDiagnosticBitmap(actual, "filled_promoted_layer_after_style_mutation.png");
  ASSERT_FALSE(actual.empty());
  EXPECT_GT(CountGreenPixels(actual), 600)
      << "A style mutation on a selected path must publish and present a Geode layer texture whose "
         "pixels include the new fill color.";
}

TEST(EditorWindowTest, WgpuPresentsFilledPenCreatedPromotedLayerAfterStyleMutation) {
  EditorWindow window(EditorWindowOptions{
      .title = "Pen-Created Filled Promoted Layer WGPU Presentation Test",
      .initialWidth = 160,
      .initialHeight = 160,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  if (!window.valid() || window.geodeDevice() == nullptr ||
      window.geodeFramebufferDevice() == nullptr) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }

  EditorApp app;
  ASSERT_TRUE(app.loadFromString(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
      <rect width="100" height="100" fill="#10131e"/>
    </svg>
  )svg"));
  app.document().document().setCanvasSize(100, 100);
  app.setActiveFill("none");
  app.setActiveStroke("black");
  app.setActiveStrokeWidth(1.0);

  svg::Renderer renderer(window.geodeDevice());
  if (!renderer.requiresTextureSnapshotPresentation()) {
    GTEST_SKIP() << "This regression is specific to Geode texture-snapshot presentation.";
  }
  AsyncRenderer asyncRenderer;

  RenderRequest warmRequest(renderer, app.document().document());
  warmRequest.version = 1;
  warmRequest.documentGeneration = app.document().documentGeneration();
  asyncRenderer.requestRender(warmRequest);
  ASSERT_TRUE(WaitForRenderResult(asyncRenderer).has_value());

  PenTool penTool;
  penTool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  penTool.onMouseDown(app, Vector2d(80.0, 10.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  penTool.onMouseDown(app, Vector2d(10.0, 80.0), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  ASSERT_TRUE(penTool.commitOpenPath(app));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_EQ(app.selectedElements().size(), 1u);
  const Entity targetEntity = app.selectedElements().front().unsafeEntityHandle().entity();

  const std::optional<RenderResult> before = RenderSelectedPromotedPreview(
      asyncRenderer, renderer, app, targetEntity, /*version=*/2, /*forceLayerRasterization=*/false);
  ASSERT_TRUE(before.has_value());
  ASSERT_TRUE(before->compositedPreview.has_value());
  const RenderResult::CompositedTile* beforeTile =
      FindLayerTile(*before->compositedPreview, targetEntity);
  ASSERT_NE(beforeTile, nullptr);
  ASSERT_NE(beforeTile->textureSnapshot, nullptr);

  ASSERT_TRUE(app.setStylePropertyOnSelection("fill", "#36c317"));
  ASSERT_TRUE(app.flushFrame());

  const std::optional<RenderResult> after = RenderSelectedPromotedPreview(
      asyncRenderer, renderer, app, targetEntity, /*version=*/3, /*forceLayerRasterization=*/true);
  ASSERT_TRUE(after.has_value());
  ASSERT_TRUE(after->compositedPreview.has_value());
  const RenderResult::CompositedTile* afterTile =
      FindLayerTile(*after->compositedPreview, targetEntity);
  ASSERT_NE(afterTile, nullptr);
  ASSERT_NE(afterTile->textureSnapshot, nullptr);
  ASSERT_GT(afterTile->generation, beforeTile->generation);

  std::shared_ptr<const svg::RendererTextureSnapshot> texture = afterTile->textureSnapshot;
  svg::RendererGeode underlayRenderer(window.geodeFramebufferDevice());
  window.setWgpuUnderlayRenderCallback(
      [&underlayRenderer, texture](const EditorWindowWgpuRenderTarget& renderTarget) {
        if (!renderTarget.texture.isValid()) {
          return;
        }

        svg::RenderViewport viewport;
        viewport.size = Vector2d(static_cast<double>(renderTarget.framebufferSizePx.x),
                                 static_cast<double>(renderTarget.framebufferSizePx.y));
        viewport.devicePixelRatio = 1.0;

        underlayRenderer.setTargetTexture(renderTarget.texture);
        underlayRenderer.setPreserveTargetOnBeginFrame(true);
        underlayRenderer.beginFrame(viewport);
        ASSERT_TRUE(underlayRenderer.drawTextureSnapshot(
            *texture, Box2d(Vector2d(16.0, 16.0),
                            Vector2d(static_cast<double>(renderTarget.framebufferSizePx.x - 16),
                                     static_cast<double>(renderTarget.framebufferSizePx.y - 16)))));
        underlayRenderer.endFrame();
        underlayRenderer.clearTargetTexture();
      });

  window.beginFrame();
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();
  WriteDiagnosticBitmap(actual, "filled_pen_created_promoted_layer_after_style_mutation.png");
  ASSERT_FALSE(actual.empty());
  EXPECT_GT(CountGreenPixels(actual), 600)
      << "A fill mutation on a Pen-created selected path must refresh the Geode promoted layer "
         "texture instead of retaining the no-fill snapshot.";
}

TEST(EditorWindowTest, WgpuPresentsFilledSplashPenLayerAfterStyleMutation) {
  EditorWindow window(EditorWindowOptions{
      .title = "Splash Pen Fill WGPU Presentation Test",
      .initialWidth = 220,
      .initialHeight = 180,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  if (!window.valid() || window.geodeDevice() == nullptr ||
      window.geodeFramebufferDevice() == nullptr) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }

  const std::string source = ReadRunfileText("donner_splash.svg");
  ASSERT_FALSE(source.empty());
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(source));
  app.document().document().setCanvasSize(892, 512);
  app.setActiveFill("none");
  app.setActiveStroke("black");
  app.setActiveStrokeWidth(1.0);

  svg::Renderer renderer(window.geodeDevice());
  if (!renderer.requiresTextureSnapshotPresentation()) {
    GTEST_SKIP() << "This regression is specific to Geode texture-snapshot presentation.";
  }
  AsyncRenderer asyncRenderer;

  RenderRequest warmRequest(renderer, app.document().document());
  warmRequest.version = 1;
  warmRequest.documentGeneration = app.document().documentGeneration();
  asyncRenderer.requestRender(warmRequest);
  ASSERT_TRUE(WaitForRenderResult(asyncRenderer).has_value());

  PenTool penTool;
  penTool.onMouseDown(app, Vector2d(313.0, 121.5), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  penTool.onMouseDown(app, Vector2d(433.0, 241.5), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  penTool.onMouseDown(app, Vector2d(276.0, 269.5), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  penTool.onMouseDown(app, Vector2d(247.0, 191.5), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  penTool.onMouseDown(app, Vector2d(313.0, 121.5), MouseModifiers{.pixelsPerDocUnit = 1.0});
  penTool.onMouseUp(app, Vector2d(313.0, 121.5));
  ASSERT_TRUE(app.flushFrame());
  ASSERT_FALSE(penTool.isDrafting());
  ASSERT_EQ(app.selectedElements().size(), 1u);
  const Entity targetEntity = app.selectedElements().front().unsafeEntityHandle().entity();

  const std::optional<RenderResult> before = RenderSelectedPromotedPreview(
      asyncRenderer, renderer, app, targetEntity, /*version=*/2, /*forceLayerRasterization=*/false);
  ASSERT_TRUE(before.has_value());
  ASSERT_TRUE(before->compositedPreview.has_value());
  const RenderResult::CompositedTile* beforeTile =
      FindLayerTile(*before->compositedPreview, targetEntity);
  ASSERT_NE(beforeTile, nullptr);
  ASSERT_NE(beforeTile->textureSnapshot, nullptr);

  ASSERT_TRUE(app.setStylePropertyOnSelection("fill", "#36c317"));
  ASSERT_TRUE(app.flushFrame());

  const std::optional<RenderResult> after = RenderSelectedPromotedPreview(
      asyncRenderer, renderer, app, targetEntity, /*version=*/3, /*forceLayerRasterization=*/true);
  ASSERT_TRUE(after.has_value());
  ASSERT_TRUE(after->compositedPreview.has_value());
  const RenderResult::CompositedTile* afterTile =
      FindLayerTile(*after->compositedPreview, targetEntity);
  ASSERT_NE(afterTile, nullptr);
  ASSERT_NE(afterTile->textureSnapshot, nullptr);
  ASSERT_GT(afterTile->generation, beforeTile->generation);

  std::shared_ptr<const svg::RendererTextureSnapshot> texture = afterTile->textureSnapshot;
  svg::RendererGeode underlayRenderer(window.geodeFramebufferDevice());
  window.setWgpuUnderlayRenderCallback(
      [&underlayRenderer, texture](const EditorWindowWgpuRenderTarget& renderTarget) {
        if (!renderTarget.texture.isValid()) {
          return;
        }

        svg::RenderViewport viewport;
        viewport.size = Vector2d(static_cast<double>(renderTarget.framebufferSizePx.x),
                                 static_cast<double>(renderTarget.framebufferSizePx.y));
        viewport.devicePixelRatio = 1.0;

        underlayRenderer.setTargetTexture(renderTarget.texture);
        underlayRenderer.setPreserveTargetOnBeginFrame(true);
        underlayRenderer.beginFrame(viewport);
        ASSERT_TRUE(underlayRenderer.drawTextureSnapshot(
            *texture, Box2d(Vector2d(16.0, 16.0),
                            Vector2d(static_cast<double>(renderTarget.framebufferSizePx.x - 16),
                                     static_cast<double>(renderTarget.framebufferSizePx.y - 16)))));
        underlayRenderer.endFrame();
        underlayRenderer.clearTargetTexture();
      });

  window.beginFrame();
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();
  WriteDiagnosticBitmap(actual, "filled_splash_pen_layer_after_style_mutation.png");
  ASSERT_FALSE(actual.empty());
  EXPECT_GT(CountGreenPixels(actual), 600)
      << "The Donner splash compositor must publish the new fill pixels for the Pen-created "
         "selected layer.";
}

TEST(EditorWindowTest, WgpuPenFillReplayViewportPublishesFilledLayerTile) {
  EditorWindow window(EditorWindowOptions{
      .title = "Pen Fill Replay Viewport Layer Texture Test",
      .initialWidth = 220,
      .initialHeight = 180,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  if (!window.valid() || window.geodeDevice() == nullptr) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }

  const std::string source = ReadRunfileText("donner_splash.svg");
  ASSERT_FALSE(source.empty());
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(source));
  app.document().document().setCanvasSize(892, 512);
  app.setActiveFill("none");
  app.setActiveStroke("black");
  app.setActiveStrokeWidth(1.0);

  svg::Renderer renderer(window.geodeDevice());
  if (!renderer.requiresTextureSnapshotPresentation()) {
    GTEST_SKIP() << "This regression is specific to Geode texture-snapshot presentation.";
  }
  AsyncRenderer asyncRenderer;

  RenderRequest warmRequest(renderer, app.document().document());
  warmRequest.version = 1;
  warmRequest.documentGeneration = app.document().documentGeneration();
  asyncRenderer.requestRender(warmRequest);
  ASSERT_TRUE(WaitForRenderResult(asyncRenderer).has_value());

  PenTool penTool;
  penTool.onMouseDown(app, Vector2d(392.0, 228.5), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  penTool.onMouseDown(app, Vector2d(501.0, 320.5), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  penTool.onMouseDown(app, Vector2d(322.0, 337.5), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  penTool.onMouseDown(app, Vector2d(340.0, 266.5), MouseModifiers{});
  ASSERT_TRUE(app.flushFrame());
  penTool.onMouseDown(app, Vector2d(392.0, 228.5), MouseModifiers{.pixelsPerDocUnit = 1.0});
  ASSERT_TRUE(app.flushFrame());
  ASSERT_EQ(app.selectedElements().size(), 1u);
  const Entity targetEntity = app.selectedElements().front().unsafeEntityHandle().entity();

  const EditorRasterViewport replayRasterViewport =
      PenFillReplayViewport().selectedPrewarmRasterViewport();
  const std::optional<RenderResult> before =
      RenderSelectedPromotedPreview(asyncRenderer, renderer, app, targetEntity, /*version=*/2,
                                    /*forceLayerRasterization=*/false, replayRasterViewport);
  ASSERT_TRUE(before.has_value());
  ASSERT_TRUE(before->compositedPreview.has_value());
  const RenderResult::CompositedTile* beforeTile =
      FindLayerTile(*before->compositedPreview, targetEntity);
  ASSERT_NE(beforeTile, nullptr);
  ASSERT_NE(beforeTile->textureSnapshot, nullptr);

  ASSERT_TRUE(app.setStylePropertyOnSelection("fill", "#36c317"));
  ASSERT_TRUE(app.flushFrame());

  const std::optional<RenderResult> after =
      RenderSelectedPromotedPreview(asyncRenderer, renderer, app, targetEntity, /*version=*/3,
                                    /*forceLayerRasterization=*/true, replayRasterViewport);
  ASSERT_TRUE(after.has_value());
  ASSERT_TRUE(after->compositedPreview.has_value());
  const RenderResult::CompositedTile* afterTile =
      FindLayerTile(*after->compositedPreview, targetEntity);
  ASSERT_NE(afterTile, nullptr);
  ASSERT_NE(afterTile->textureSnapshot, nullptr);
  ASSERT_GT(afterTile->generation, beforeTile->generation);

  const Box2d pathBoundsDoc = Box2d::FromXYWH(322.0, 228.5, 179.0, 109.0);
  ExpectTileEnclosesPathBounds(*afterTile, pathBoundsDoc);

  std::shared_ptr<const svg::RendererTextureSnapshot> texture = afterTile->textureSnapshot;
  svg::RendererGeode underlayRenderer(window.geodeFramebufferDevice());
  window.setWgpuUnderlayRenderCallback(
      [&underlayRenderer, texture](const EditorWindowWgpuRenderTarget& renderTarget) {
        if (!renderTarget.texture.isValid()) {
          return;
        }

        svg::RenderViewport viewport;
        viewport.size = Vector2d(static_cast<double>(renderTarget.framebufferSizePx.x),
                                 static_cast<double>(renderTarget.framebufferSizePx.y));
        viewport.devicePixelRatio = 1.0;

        underlayRenderer.setTargetTexture(renderTarget.texture);
        underlayRenderer.setPreserveTargetOnBeginFrame(true);
        underlayRenderer.beginFrame(viewport);
        ASSERT_TRUE(underlayRenderer.drawTextureSnapshot(
            *texture, Box2d(Vector2d(16.0, 16.0),
                            Vector2d(static_cast<double>(renderTarget.framebufferSizePx.x - 16),
                                     static_cast<double>(renderTarget.framebufferSizePx.y - 16)))));
        underlayRenderer.endFrame();
        underlayRenderer.clearTargetTexture();
      });

  window.beginFrame();
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();
  WriteDiagnosticBitmap(actual, "pen_fill_replay_viewport_layer_texture.png");
  ASSERT_FALSE(actual.empty());
  EXPECT_GT(CountGreenPixels(actual), 600)
      << "The promoted-layer texture for the replay path must contain the new fill color.";
}

TEST(EditorWindowTest, WgpuPenFillLiveSourceSyncPublishesFilledLayerTile) {
  EditorWindow window(EditorWindowOptions{
      .title = "Pen Fill Live Source Sync Layer Texture Test",
      .initialWidth = 220,
      .initialHeight = 180,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  if (!window.valid() || window.geodeDevice() == nullptr ||
      window.geodeFramebufferDevice() == nullptr) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }

  constexpr std::string_view kSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="80" height="80"></svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSource));
  app.document().document().setCanvasSize(80, 80);
  app.setActiveFill("none");
  app.setActiveStroke("black");
  app.setActiveStrokeWidth(1.0);

  TextEditor textEditor;
  textEditor.setText(kSource);
  textEditor.resetTextChanged();
  SelectTool selectTool;
  DocumentSyncController syncController{std::string(kSource)};
  auto frame = [&]() {
    app.flushFrame();
    syncController.syncParseErrorMarkers(app, textEditor);
    syncController.applyPendingWritebacks(app, selectTool, textEditor);
    syncController.handleTextEdits(app, textEditor, /*deltaSeconds=*/1.0f);
  };
  auto settle = [&]() {
    for (int i = 0; i < 8; ++i) {
      frame();
    }
  };

  svg::Renderer renderer(window.geodeDevice());
  if (!renderer.requiresTextureSnapshotPresentation()) {
    GTEST_SKIP() << "This regression is specific to Geode texture-snapshot presentation.";
  }
  AsyncRenderer asyncRenderer;

  RenderRequest warmRequest(renderer, app.document().document());
  warmRequest.version = 1;
  warmRequest.documentGeneration = app.document().documentGeneration();
  asyncRenderer.requestRender(warmRequest);
  ASSERT_TRUE(WaitForRenderResult(asyncRenderer).has_value());

  PenTool penTool;
  penTool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{});
  frame();
  penTool.onMouseDown(app, Vector2d(70.0, 10.0), MouseModifiers{});
  frame();
  penTool.onMouseDown(app, Vector2d(40.0, 70.0), MouseModifiers{});
  frame();
  penTool.onMouseDown(app, Vector2d(10.0, 10.0), MouseModifiers{.pixelsPerDocUnit = 1.0});
  penTool.onMouseUp(app, Vector2d(10.0, 10.0));
  settle();
  ASSERT_FALSE(penTool.isDrafting());
  ASSERT_EQ(app.selectedElements().size(), 1u);
  const Entity targetEntity = app.selectedElements().front().unsafeEntityHandle().entity();

  const std::optional<RenderResult> before = RenderSelectedPromotedPreview(
      asyncRenderer, renderer, app, targetEntity, /*version=*/2, /*forceLayerRasterization=*/false);
  ASSERT_TRUE(before.has_value());
  ASSERT_TRUE(before->compositedPreview.has_value());
  const RenderResult::CompositedTile* beforeTile =
      FindLayerTile(*before->compositedPreview, targetEntity);
  ASSERT_NE(beforeTile, nullptr);
  ASSERT_NE(beforeTile->textureSnapshot, nullptr);

  ASSERT_TRUE(app.setStylePropertyOnSelection("fill", "#36c317"));
  settle();

  const std::string source(app.document().document().source());
  EXPECT_NE(source.find("fill: #36c317"), std::string::npos)
      << "The live source-sync path should mirror the selected path fill into source:\n"
      << source;

  const std::optional<RenderResult> after = RenderSelectedPromotedPreview(
      asyncRenderer, renderer, app, targetEntity, /*version=*/3, /*forceLayerRasterization=*/true);
  ASSERT_TRUE(after.has_value());
  ASSERT_TRUE(after->compositedPreview.has_value());
  const RenderResult::CompositedTile* afterTile =
      FindLayerTile(*after->compositedPreview, targetEntity);
  ASSERT_NE(afterTile, nullptr);
  ASSERT_NE(afterTile->textureSnapshot, nullptr);
  ASSERT_GT(afterTile->generation, beforeTile->generation);

  std::shared_ptr<const svg::RendererTextureSnapshot> texture = afterTile->textureSnapshot;
  svg::RendererGeode underlayRenderer(window.geodeFramebufferDevice());
  window.setWgpuUnderlayRenderCallback(
      [&underlayRenderer, texture](const EditorWindowWgpuRenderTarget& renderTarget) {
        if (!renderTarget.texture.isValid()) {
          return;
        }

        svg::RenderViewport viewport;
        viewport.size = Vector2d(static_cast<double>(renderTarget.framebufferSizePx.x),
                                 static_cast<double>(renderTarget.framebufferSizePx.y));
        viewport.devicePixelRatio = 1.0;

        underlayRenderer.setTargetTexture(renderTarget.texture);
        underlayRenderer.setPreserveTargetOnBeginFrame(true);
        underlayRenderer.beginFrame(viewport);
        ASSERT_TRUE(underlayRenderer.drawTextureSnapshot(
            *texture, Box2d(Vector2d(16.0, 16.0),
                            Vector2d(static_cast<double>(renderTarget.framebufferSizePx.x - 16),
                                     static_cast<double>(renderTarget.framebufferSizePx.y - 16)))));
        underlayRenderer.endFrame();
        underlayRenderer.clearTargetTexture();
      });

  window.beginFrame();
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();
  WriteDiagnosticBitmap(actual, "pen_fill_live_source_sync_layer_texture.png");
  ASSERT_FALSE(actual.empty());
  EXPECT_GT(CountGreenPixels(actual), 600)
      << "The promoted-layer texture for a live source-synced Pen path must contain the fill "
         "chosen through the UI.";
}

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
  ScopedUiTexture uiTexture(*texture, UiTextureAlphaMode::Premultiplied);
  const ImTextureID textureId = uiTexture.id();
  ASSERT_NE(textureId, 0);

  window.beginFrame();
  ImGui::GetBackgroundDrawList()->AddImage(textureId, ImVec2(16.0f, 16.0f), ImVec2(80.0f, 80.0f));
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();

  ASSERT_FALSE(actual.empty());
  const std::array<std::uint8_t, 4> center = PixelAt(actual, 48, 48);
  EXPECT_THAT(center, Rgba(Near(128, 3), testing::Le(3), testing::Le(3), testing::Eq(255)))
      << "A premultiplied red texture should not be multiplied by alpha again during ImGui "
         "presentation.";
}

svg::compositor::CompositorController::CompositeTileSnapshot CpuDebugThumbnail() {
  svg::compositor::CompositorController::CompositeTileSnapshot tile;
  tile.kind = decltype(tile)::Kind::Segment;
  tile.id = "seg:cpu-upload";
  tile.generation = 1;
  tile.hasValidBitmap = true;
  tile.thumbnailDims = Vector2i(3, 2);
  tile.thumbnailPixels = {255, 0, 0, 128, 255, 0, 0, 128, 255, 0, 0, 128,
                          255, 0, 0, 128, 255, 0, 0, 128, 255, 0, 0, 128};
  return tile;
}

TEST(EditorWindowTest, CompositorDebugPanelUploadsCpuThumbnailThroughRuntime) {
  EditorWindow window(EditorWindowOptions{
      .title = "Compositor CPU Thumbnail Test",
      .initialWidth = 96,
      .initialHeight = 96,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  ASSERT_TRUE(window.valid());
  const std::shared_ptr<geode::GeodeDevice> device = window.geodeFramebufferDevice();
  ASSERT_NE(device, nullptr);
  CompositorDebugPanel panel(device);
  auto tile = CpuDebugThumbnail();

  const uint64_t submittedBefore = device->runtimeDevice().lastSubmittedSerial();
  geode::GeodeCounters counters;
  device->setCounters(&counters);
  const ImTextureID texture = CompositorDebugPanelTestAccess::upload(panel, tile);
  device->setCounters(nullptr);
  ASSERT_NE(texture, 0u);
  EXPECT_EQ(counters.textureCreates, 1u);
  EXPECT_EQ(counters.textureWriteBytes, 256u * 2u);
  EXPECT_EQ(counters.submits, 0u);
  EXPECT_EQ(device->runtimeDevice().lastSubmittedSerial(), submittedBefore);

  window.beginFrame();
  ImGui::GetBackgroundDrawList()->AddImage(texture, ImVec2(16, 16), ImVec2(48, 48));
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();
  ASSERT_FALSE(actual.empty());
  EXPECT_THAT(PixelAt(actual, 32, 32),
              Rgba(Near(128, 3), testing::Le(3), testing::Le(3), testing::Eq(255)));
}

TEST(EditorWindowTest, CompositorDebugPanelReusesUnchangedCpuThumbnail) {
  EditorWindow window(EditorWindowOptions{
      .title = "Compositor CPU Thumbnail Test",
      .initialWidth = 96,
      .initialHeight = 96,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  ASSERT_TRUE(window.valid());
  const std::shared_ptr<geode::GeodeDevice> device = window.geodeFramebufferDevice();
  ASSERT_NE(device, nullptr);
  CompositorDebugPanel panel(device);
  auto tile = CpuDebugThumbnail();

  const ImTextureID first = CompositorDebugPanelTestAccess::upload(panel, tile);
  ASSERT_NE(first, 0u);
  UiTextureRegistry* registry = CurrentUiTextureRegistry();
  ASSERT_NE(registry, nullptr);
  const size_t liveBefore = registry->liveCount();
  geode::GeodeCounters counters;
  device->setCounters(&counters);
  const ImTextureID second = CompositorDebugPanelTestAccess::upload(panel, tile);
  device->setCounters(nullptr);
  EXPECT_EQ(second, first);
  EXPECT_EQ(registry->liveCount(), liveBefore);
  EXPECT_EQ(counters.textureCreates, 0u);
  EXPECT_EQ(counters.textureWriteBytes, 0u);
  EXPECT_EQ(counters.submits, 0u);
}

TEST(EditorWindowTest, CompositorDebugPanelFailedReplacementKeepsPreviousPreview) {
  EditorWindow window(EditorWindowOptions{
      .title = "Compositor CPU Thumbnail Test",
      .initialWidth = 96,
      .initialHeight = 96,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  ASSERT_TRUE(window.valid());
  const std::shared_ptr<geode::GeodeDevice> device = window.geodeFramebufferDevice();
  ASSERT_NE(device, nullptr);
  CompositorDebugPanel panel(device);
  auto tile = CpuDebugThumbnail();

  const ImTextureID first = CompositorDebugPanelTestAccess::upload(panel, tile);
  ASSERT_NE(first, 0u);
  UiTextureRegistry* registry = CurrentUiTextureRegistry();
  ASSERT_NE(registry, nullptr);
  const size_t liveBefore = registry->liveCount();
  ++tile.generation;
  tile.thumbnailPixels.pop_back();
  geode::GeodeCounters counters;
  device->setCounters(&counters);
  const ImTextureID refused = CompositorDebugPanelTestAccess::upload(panel, tile);
  device->setCounters(nullptr);
  EXPECT_EQ(refused, first);
  EXPECT_EQ(registry->liveCount(), liveBefore);
  EXPECT_EQ(counters.textureCreates, 0u);
  EXPECT_EQ(counters.textureWriteBytes, 0u);
  EXPECT_EQ(counters.submits, 0u);
  for (int frame = 0; frame < 5; ++frame) {
    panel.advancePresentationFrame();
  }
  EXPECT_TRUE(registry->lookup(UiTextureId::FromImTextureId(first)).hasResult());
  window.beginFrame();
  ImGui::GetBackgroundDrawList()->AddImage(first, ImVec2(16, 16), ImVec2(48, 48));
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();
  ASSERT_FALSE(actual.empty());
  EXPECT_THAT(PixelAt(actual, 32, 32),
              Rgba(Near(128, 3), testing::Le(3), testing::Le(3), testing::Eq(255)));
}

// A thumbnail whose extent has no area cannot be presented at all, so the panel drops whatever it
// published for that tile rather than leaving the previous preview on screen indefinitely.
TEST(EditorWindowTest, CompositorDebugPanelDegenerateThumbnailExtentDropsThePreview) {
  EditorWindow window(EditorWindowOptions{
      .title = "Compositor CPU Thumbnail Test",
      .initialWidth = 96,
      .initialHeight = 96,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  ASSERT_TRUE(window.valid());
  const std::shared_ptr<geode::GeodeDevice> device = window.geodeFramebufferDevice();
  ASSERT_NE(device, nullptr);
  CompositorDebugPanel panel(device);
  auto tile = CpuDebugThumbnail();

  const ImTextureID first = CompositorDebugPanelTestAccess::upload(panel, tile);
  ASSERT_NE(first, 0u);
  UiTextureRegistry* registry = CurrentUiTextureRegistry();
  ASSERT_NE(registry, nullptr);
  const size_t liveBefore = registry->liveCount();

  ++tile.generation;
  tile.thumbnailDims = Vector2i(0, 2);
  geode::GeodeCounters counters;
  device->setCounters(&counters);
  const ImTextureID degenerate = CompositorDebugPanelTestAccess::upload(panel, tile);
  device->setCounters(nullptr);
  EXPECT_THAT(degenerate, testing::Eq(ImTextureID(0)))
      << "A zero-area thumbnail extent has nothing to present, so the tile's registration must be "
         "dropped instead of republishing the stale preview.";
  EXPECT_THAT(counters.textureCreates, testing::Eq(0u));
  EXPECT_THAT(counters.textureWriteBytes, testing::Eq(0u));

  for (int frame = 0; frame < 4; ++frame) {
    panel.advancePresentationFrame();
  }
  EXPECT_THAT(registry->lookup(UiTextureId::FromImTextureId(first)).hasError(), testing::IsTrue())
      << "The dropped registration must be released once its presentation window closes.";
  EXPECT_THAT(registry->liveCount(), testing::Eq(liveBefore - 1u));
}

TEST(EditorWindowTest, CompositorDebugPanelRetiresReplacedUploadAfterPresentationWindow) {
  EditorWindow window(EditorWindowOptions{
      .title = "Compositor CPU Thumbnail Test",
      .initialWidth = 96,
      .initialHeight = 96,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  ASSERT_TRUE(window.valid());
  const std::shared_ptr<geode::GeodeDevice> device = window.geodeFramebufferDevice();
  ASSERT_NE(device, nullptr);
  CompositorDebugPanel panel(device);
  auto tile = CpuDebugThumbnail();

  const ImTextureID first = CompositorDebugPanelTestAccess::upload(panel, tile);
  ASSERT_NE(first, 0u);
  UiTextureRegistry* registry = CurrentUiTextureRegistry();
  ASSERT_NE(registry, nullptr);
  const size_t liveBefore = registry->liveCount();
  ++tile.generation;
  const ImTextureID second = CompositorDebugPanelTestAccess::upload(panel, tile);
  ASSERT_NE(second, 0u);
  EXPECT_NE(second, first);
  EXPECT_EQ(registry->liveCount(), liveBefore + 1u);
  for (int frame = 0; frame < 3; ++frame) {
    panel.advancePresentationFrame();
    EXPECT_TRUE(registry->lookup(UiTextureId::FromImTextureId(first)).hasResult());
  }
  panel.advancePresentationFrame();
  EXPECT_TRUE(registry->lookup(UiTextureId::FromImTextureId(first)).hasError());
  EXPECT_TRUE(registry->lookup(UiTextureId::FromImTextureId(second)).hasResult());
  EXPECT_EQ(registry->liveCount(), liveBefore);
}

TEST(EditorWindowTest, CompositorDebugPanelReusesAnUnchangedSnapshotRegistration) {
  EditorWindow window(EditorWindowOptions{
      .title = "Compositor Debug Panel Registration Reuse Test",
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
  viewport.size = Vector2d(32.0, 32.0);
  viewport.devicePixelRatio = 1.0;
  source.beginFrame(viewport);
  svg::PaintParams paint;
  paint.fill = svg::PaintServer::Solid{css::Color(css::RGBA(0, 255, 0, 255))};
  source.setPaint(paint);
  source.drawRect(Box2d({0.0, 0.0}, {32.0, 32.0}), svg::StrokeParams{});
  source.endFrame();

  const std::shared_ptr<const svg::RendererTextureSnapshot> textureSnapshot =
      source.takeTextureSnapshot();
  ASSERT_TRUE(textureSnapshot != nullptr);

  UiTextureRegistry* registry = CurrentUiTextureRegistry();
  ASSERT_TRUE(registry != nullptr);

  using CompositeTileSnapshot = svg::compositor::CompositorController::CompositeTileSnapshot;
  CompositeTileSnapshot tile;
  tile.kind = CompositeTileSnapshot::Kind::Segment;
  tile.id = "seg:reuse";
  tile.label = "segment reuse";
  tile.generation = 1;
  tile.bitmapDims = Vector2i(32, 32);
  tile.textureSnapshot = textureSnapshot;
  const std::vector<CompositeTileSnapshot> tiles{tile};

  CompositorDebugPanel panel(window.geodeDevice());
  const auto renderPanelFrame = [&]() {
    window.beginFrame();
    ImGui::Begin("##registration_reuse_host");
    panel.render(tiles, svg::compositor::CompositorController::StateSnapshot{}, entt::null,
                 /*viewportZoom=*/1.0, /*viewportDpr=*/1.0, Vector2i(96, 96), Vector2i(96, 96),
                 PresentationCoverageDiagnostics{},
                 svg::compositor::CompositorController::FastPathCounters{},
                 svg::compositor::CompositorController::RenderFrameStats{});
    ImGui::End();
    window.endFrame();
  };

  renderPanelFrame();
  const size_t afterFirstFrame = registry->liveCount();
  EXPECT_GT(afterFirstFrame, 0u) << "the tile's snapshot should have been registered once";

  renderPanelFrame();

  // Deriving the identifier used to be an address cast, so calling it every frame cost nothing.
  // Resolving through the registry allocates, and the unchanged-snapshot branch keeps the
  // identifier the entry already published, so a second registration would be attached to no
  // entry and retired by nobody: one leaked slot and backing per visible tile per frame.
  EXPECT_EQ(registry->liveCount(), afterFirstFrame)
      << "a tile whose snapshot did not change must reuse its registration";
}

TEST(EditorWindowTest, WgpuPresentsUploadedStraightAlphaBitmapWithStraightBlend) {
  EditorWindow window(EditorWindowOptions{
      .title = "Straight Alpha WGPU Bitmap Upload Test",
      .initialWidth = 96,
      .initialHeight = 96,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  if (!window.valid() || window.geodeDevice() == nullptr) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }

  svg::RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(32, 32);
  bitmap.rowBytes = static_cast<std::size_t>(bitmap.dimensions.x) * 4u;
  bitmap.alphaType = svg::AlphaType::Unpremultiplied;
  bitmap.pixels.resize(bitmap.rowBytes * static_cast<std::size_t>(bitmap.dimensions.y));
  for (std::size_t offset = 0; offset + 3 < bitmap.pixels.size(); offset += 4u) {
    bitmap.pixels[offset + 0] = 255u;
    bitmap.pixels[offset + 1] = 0u;
    bitmap.pixels[offset + 2] = 0u;
    bitmap.pixels[offset + 3] = 128u;
  }

  RenderResult::CompositedPreview preview;
  RenderResult::CompositedTile tile;
  tile.kind = RenderResult::CompositedTile::Kind::Immediate;
  tile.id = "seg:0";
  tile.generation = 1;
  tile.bitmap = std::move(bitmap);
  tile.bitmapDimsPx = tile.bitmap.dimensions;
  tile.rasterCanvasSize = Vector2i(32, 32);
  tile.bitmapDimsDoc = Vector2d(32.0, 32.0);
  preview.tiles.push_back(std::move(tile));

  GlTextureCache textures(window.geodeDevice());
  textures.initialize();
  textures.uploadComposited(preview);
  ASSERT_EQ(textures.tiles().size(), 1u);
  ASSERT_NE(textures.tiles().front().texture, 0);

  window.beginFrame();
  ImGui::GetBackgroundDrawList()->AddImage(textures.tiles().front().texture, ImVec2(16.0f, 16.0f),
                                           ImVec2(48.0f, 48.0f));
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();

  ASSERT_FALSE(actual.empty());
  const std::array<std::uint8_t, 4> center = PixelAt(actual, 32, 32);
  EXPECT_THAT(center, Rgba(Near(128, 3), testing::Le(3), testing::Le(3), testing::Eq(255)))
      << "CPU bitmap uploads are straight-alpha RGBA; registering them as premultiplied makes "
         "the WGPU presentation path skip the required source-alpha multiply.";
}

TEST(EditorWindowTest, WgpuLayerThumbnailUploadHonorsPayloadUv) {
  EditorWindow window(EditorWindowOptions{
      .title = "Layer Thumbnail WGPU UV Test",
      .initialWidth = 96,
      .initialHeight = 96,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  if (!window.valid() || window.geodeDevice() == nullptr) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }

  svg::RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(42, 24);
  bitmap.rowBytes = static_cast<std::size_t>(bitmap.dimensions.x) * 4u;
  bitmap.alphaType = svg::AlphaType::Unpremultiplied;
  bitmap.pixels.resize(bitmap.rowBytes * static_cast<std::size_t>(bitmap.dimensions.y));
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const std::size_t offset =
          static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
      if (x < 14) {
        bitmap.pixels[offset + 0] = 255u;
      } else if (x < 28) {
        bitmap.pixels[offset + 1] = 255u;
      } else {
        bitmap.pixels[offset + 2] = 255u;
      }
      bitmap.pixels[offset + 3] = 255u;
    }
  }

  GlTextureCache textures(window.geodeDevice());
  textures.initialize();
  const GlTextureCache::ThumbnailTextureView uploaded =
      textures.uploadThumbnail(/*key=*/123u, bitmap);
  ASSERT_NE(uploaded.texture, 0);
  EXPECT_NEAR(uploaded.uvBottomRight.x, 42.0 / 64.0, 0.001);
  EXPECT_NEAR(uploaded.uvBottomRight.y, 0.75, 0.001);

  window.beginFrame();
  ImGui::GetBackgroundDrawList()->AddImage(uploaded.texture, ImVec2(16.0f, 16.0f),
                                           ImVec2(58.0f, 40.0f), ImVec2(0.0f, 0.0f),
                                           ImVec2(static_cast<float>(uploaded.uvBottomRight.x),
                                                  static_cast<float>(uploaded.uvBottomRight.y)));
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();

  ASSERT_FALSE(actual.empty());
  // ImGui draw coordinates are logical; the readback is in device pixels. On a
  // 2x host this window reads back 192x192, so sampling raw framebuffer
  // coordinates lands above the drawn quad's top edge, on the clear color.
  const Vector2d readbackFromLogical = ReadbackScale(actual, 96, 96);
  const std::array<std::uint8_t, 4> center = PixelAtLogical(actual, readbackFromLogical, 37, 28);
  EXPECT_THAT(center, Rgba(testing::Le(8), testing::Ge(245), testing::Le(8), testing::Eq(255)))
      << "The middle band should stay green; sampling the full power-of-two texture instead of "
         "the payload UV shifts the blue edge padding into the center. Readback was "
      << actual.dimensions << " for a 96x96 logical window.";
}

TEST(EditorWindowTest, WgpuLayersPanelPresentsBackgroundStickerThumbnailLikeGolden) {
  EditorWindow window(EditorWindowOptions{
      .title = "Layers Panel Thumbnail Presentation Test",
      .initialWidth = 500,
      .initialHeight = 260,
      .visible = false,
      .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
  if (!window.valid() || window.geodeDevice() == nullptr) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }

  std::optional<svg::RendererBitmap> goldenBitmap = LoadRunfilePngBitmap(
      "donner/editor/tests/testdata/layer_thumbnails/donner_splash_background_sticker.png");
  ASSERT_TRUE(goldenBitmap.has_value()) << "could not load Background_sticker thumbnail golden";
  ASSERT_EQ(goldenBitmap->dimensions, Vector2i(27, 24));

  const std::string source = ReadRunfileText("donner_splash.svg");
  ASSERT_FALSE(source.empty());
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(source));

  svg::Renderer thumbnailRenderer(window.geodeDevice());
  LayersPanel panel;
  panel.refreshSnapshot(app, &thumbnailRenderer);

  const std::optional<LayerTreeRow> row = FindLayerRow(panel, "Background_sticker");
  ASSERT_TRUE(row.has_value());
  // The row thumbnail is GPU-resident on Geode and CPU pixels elsewhere; either way the panel
  // reports the same dimensions.
  const svg::RendererImage* panelThumbnail = panel.rowThumbnail(row->stableId);
  ASSERT_NE(panelThumbnail, nullptr);
  EXPECT_EQ(panelThumbnail->dimensions(), goldenBitmap->dimensions)
      << "The UI presentation test isolates ImGui/WGPU by uploading the approved golden, but "
         "the row layout must still reserve the approved thumbnail size.";

  GlTextureCache textures(window.geodeDevice());
  textures.initialize();
  GlTextureCache::ThumbnailTextureView actualUpload;
  // The panel routes each row through exactly one of the two providers
  // depending on the thumbnail payload `refreshSnapshot` produced: Geode keeps
  // thumbnails on the GPU and delivers a texture snapshot, every other backend
  // delivers a CPU bitmap. Wire both, exactly like `EditorShell` does, and
  // upload the approved golden either way so this test stays a pure ImGui/WGPU
  // presentation check.
  const auto uploadTargetRow =
      [targetStableId = row->stableId, &textures, &goldenBitmap,
       &actualUpload](std::uint64_t stableId) -> LayersPanel::ThumbnailTexture {
    if (stableId != targetStableId) {
      return LayersPanel::ThumbnailTexture{};
    }

    actualUpload = textures.uploadThumbnail(/*key=*/0xbac65001u, *goldenBitmap);
    return LayersPanel::ThumbnailTexture{
        .texture = actualUpload.texture,
        .uvBottomRight = actualUpload.uvBottomRight,
    };
  };
  const LayersPanel::ThumbnailTextureProvider textureProvider =
      [&uploadTargetRow](std::uint64_t stableId,
                         const svg::RendererBitmap&) -> LayersPanel::ThumbnailTexture {
    return uploadTargetRow(stableId);
  };
  const LayersPanel::ThumbnailTextureSnapshotProvider textureSnapshotProvider =
      [&uploadTargetRow](std::uint64_t stableId,
                         const std::shared_ptr<const svg::RendererTextureSnapshot>&)
      -> LayersPanel::ThumbnailTexture { return uploadTargetRow(stableId); };
  const GlTextureCache::ThumbnailTextureView expectedUpload =
      textures.uploadThumbnail(/*key=*/0xbac65002u, *goldenBitmap);
  ASSERT_NE(expectedUpload.texture, 0);

  EditorWindowInputOverride input;
  input.mousePosition = Vector2d(-100.0, -100.0);
  window.beginFrameWithInput(input);

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(380.0f, 230.0f), ImGuiCond_Always);
  ImGui::Begin("##layers_panel_thumbnail_presentation", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
  panel.render(&app, textureProvider, /*iconTextureProvider=*/{},
               /*minimumInteractionHeight=*/0.0f, textureSnapshotProvider);
  const std::optional<ImageDrawRect> actualRect =
      FindTextureDrawRect(*ImGui::GetWindowDrawList(), actualUpload.texture);
  ImGui::End();
  const svg::RendererBitmap actualFramebuffer = window.endFrameAndReadPixels();
  ASSERT_FALSE(actualFramebuffer.empty());
  ASSERT_TRUE(actualRect.has_value())
      << "expected the Background_sticker row to draw the uploaded thumbnail texture";

  window.beginFrameWithInput(input);
  const ImVec2 expectedMin = actualRect->min;
  const ImVec2 expectedMax = actualRect->max;
  ImDrawList* foregroundDrawList = ImGui::GetForegroundDrawList();
  DrawThumbnailCheckerboardForTest(foregroundDrawList, expectedMin, expectedMax);
  foregroundDrawList->AddImage(expectedUpload.texture, expectedMin, expectedMax, ImVec2(0.0f, 0.0f),
                               ImVec2(static_cast<float>(expectedUpload.uvBottomRight.x),
                                      static_cast<float>(expectedUpload.uvBottomRight.y)));
  foregroundDrawList->AddRect(expectedMin, expectedMax, IM_COL32(255, 255, 255, 60), 3.0f);
  const svg::RendererBitmap expectedFramebuffer = window.endFrameAndReadPixels();
  ASSERT_FALSE(expectedFramebuffer.empty());

  const Vector2d actualReadbackFromLogical = ReadbackScale(actualFramebuffer, 500, 260);
  const Vector2d expectedReadbackFromLogical = ReadbackScale(expectedFramebuffer, 500, 260);
  const svg::RendererBitmap actualCrop =
      CropLogicalRect(actualFramebuffer, actualReadbackFromLogical, *actualRect);
  const svg::RendererBitmap expectedCrop =
      CropLogicalRect(expectedFramebuffer, expectedReadbackFromLogical, *actualRect);
  WriteDiagnosticBitmap(actualCrop, "actual_background_sticker_layer_ui_crop.png");
  WriteDiagnosticBitmap(expectedCrop, "expected_background_sticker_layer_ui_crop.png");
  ASSERT_EQ(actualCrop.dimensions, expectedCrop.dimensions)
      << "The Layers-panel row must reserve the same presented crop size as the approved "
         "Background_sticker thumbnail.";

  const std::optional<PixelBounds> actualContentBounds = DarkContentBounds(actualCrop);
  const std::optional<PixelBounds> expectedContentBounds = DarkContentBounds(expectedCrop);
  ASSERT_TRUE(actualContentBounds.has_value()) << "actual UI crop should contain sticker content";
  ASSERT_TRUE(expectedContentBounds.has_value()) << "expected golden crop should contain content";
  EXPECT_EQ(actualContentBounds->minX, expectedContentBounds->minX);
  EXPECT_EQ(actualContentBounds->minY, expectedContentBounds->minY);
  EXPECT_EQ(actualContentBounds->maxX, expectedContentBounds->maxX);
  EXPECT_EQ(actualContentBounds->maxY, expectedContentBounds->maxY);
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

  // Both owners present the same shared premultiplied snapshot via a one-tile
  // composited upload. Retiring it from one owner must not drop the shared
  // premultiplied-alpha registration while the other still draws it.
  const auto sharedSnapshotPreview = [&texture]() {
    RenderResult::CompositedPreview preview;
    RenderResult::CompositedTile tile;
    tile.kind = RenderResult::CompositedTile::Kind::Layer;
    tile.id = "layer:0";
    tile.generation = 1;
    tile.textureSnapshot = texture;
    tile.bitmapDimsPx = texture->dimensions();
    tile.rasterCanvasSize = texture->dimensions();
    tile.bitmapDimsDoc = Vector2d(texture->dimensions().x, texture->dimensions().y);
    preview.tiles.push_back(std::move(tile));
    return preview;
  };

  GlTextureCache liveOwner(window.geodeDevice());
  GlTextureCache transientOwner(window.geodeDevice());
  liveOwner.initialize();
  transientOwner.initialize();
  liveOwner.uploadComposited(sharedSnapshotPreview());
  transientOwner.uploadComposited(sharedSnapshotPreview());
  ASSERT_EQ(liveOwner.tiles().size(), 1u);
  ASSERT_NE(liveOwner.tiles().front().texture, 0);
  const ImTextureID liveTexture = liveOwner.tiles().front().texture;

  window.beginFrame();
  ImGui::GetBackgroundDrawList()->AddImage(liveTexture, ImVec2(16.0f, 16.0f), ImVec2(48.0f, 48.0f));
  const svg::RendererBitmap expected = window.endFrameAndReadPixels();
  WriteDiagnosticBitmap(expected, "premul_shared_owner_before_retire_presentation.png");

  transientOwner.resetComposited();
  for (int i = 0; i < 5; ++i) {
    transientOwner.advancePresentationFrame();
  }

  window.beginFrame();
  ImGui::GetBackgroundDrawList()->AddImage(liveTexture, ImVec2(16.0f, 16.0f), ImVec2(48.0f, 48.0f));
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();
  WriteDiagnosticBitmap(actual, "premul_shared_owner_retire_presentation.png");

  ASSERT_FALSE(actual.empty());
  const Vector2d readbackFromLogical = ReadbackScale(actual, 128, 96);
  const std::array<std::uint8_t, 4> center =
      PixelAtLogical(actual, readbackFromLogical, 32.0, 32.0);
  EXPECT_THAT(center, Rgba(Near(128, 3), testing::Le(3), testing::Le(3), testing::Eq(255)))
      << "One presentation owner retiring a shared Geode texture must not clear the "
         "premultiplied-alpha registration while another owner still draws it.";
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
  ScopedUiTexture uiTexture(*texture, UiTextureAlphaMode::Premultiplied);
  const ImTextureID textureId = uiTexture.id();
  ASSERT_NE(textureId, 0);

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
  // Required before the device check: a host without WebGPU still has the runfiles tree, so a
  // removed or misdeclared entry must not ride out as a green skip on that host.
  const donner::tests::RequiredRunfile splashFile =
      donner::tests::ReadRequiredRunfile("donner_splash.svg");
  DONNER_REQUIRE_RUNFILE(splashFile);

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

  std::optional<svg::SVGDocument> document = ParseDonnerSplashDocument(splashFile.contents);
  ASSERT_TRUE(document.has_value());

  std::optional<svg::SVGElement> target = document->querySelector("#Big_lightning_glow");
  ASSERT_TRUE(target.has_value());
  std::optional<RenderResult::CompositedPreview> preview = RenderCompositedPreview(
      window.geodeDevice(), *document, target->unsafeEntityHandle().entity());
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
