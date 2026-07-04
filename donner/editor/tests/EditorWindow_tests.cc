#include "donner/editor/gui/EditorWindow.h"

#include <gmock/gmock.h>
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
#include "donner/base/tests/Runfiles.h"
#include "donner/css/Color.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/DocumentSyncController.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/LayersPanel.h"
#include "donner/editor/PenTool.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/TextEditor.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/tests/BitmapTestMatchers.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/StrokeParams.h"
#include "donner/svg/renderer/tests/RendererImageTestUtils.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#endif

namespace donner::editor::gui {
namespace {

#if defined(DONNER_EDITOR_WGPU)
using ::donner::editor::tests::NonEmptyRendererBitmap;
using svg::test::Near;
using svg::test::Rgba;
using ::testing::SizeIs;

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
  std::ifstream splashStream(donner::Runfiles::instance().Rlocation("donner_splash.svg"));
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
TEST(EditorWindowTest, WgpuDirectRenderCallbackAppendsToFramebuffer) {
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
    if (!target.texture) {
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
    directRenderer.drawRect(Box2d(Vector2d(32.0, 32.0) * framebufferFromLogical,
                                  Vector2d(64.0, 64.0) * framebufferFromLogical),
                            svg::StrokeParams{});
    directRenderer.endFrame();
    directRenderer.clearTargetTexture();
  });

  window.beginFrame();
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(96.0f, 96.0f),
                                                IM_COL32(0, 0, 255, 255));
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();
  ASSERT_THAT(actual, NonEmptyRendererBitmap());
  const Vector2d readbackFromLogical = ReadbackScale(actual, 96, 96);

  const std::array<std::uint8_t, 4> outside = PixelAtLogical(actual, readbackFromLogical, 16, 16);
  EXPECT_THAT(outside, Rgba(testing::Le(3), testing::Le(3), Near(255, 3), testing::Eq(255)))
      << "The direct Geode pass must preserve earlier ImGui framebuffer pixels.";

  const std::array<std::uint8_t, 4> inside = PixelAtLogical(actual, readbackFromLogical, 48, 48);
  EXPECT_THAT(inside, Rgba(Near(255, 3), testing::Le(3), testing::Le(3), testing::Eq(255)));
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
        if (!target.texture) {
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
  ASSERT_THAT(actual, NonEmptyRendererBitmap());
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
        if (!renderTarget.texture) {
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
  ASSERT_THAT(actual, NonEmptyRendererBitmap());
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
  ASSERT_THAT(app.selectedElements(), SizeIs(1u));
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
        if (!renderTarget.texture) {
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
  ASSERT_THAT(actual, NonEmptyRendererBitmap());
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
  ASSERT_TRUE(app.flushFrame());
  ASSERT_FALSE(penTool.isDrafting());
  ASSERT_THAT(app.selectedElements(), SizeIs(1u));
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
        if (!renderTarget.texture) {
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
  ASSERT_THAT(actual, NonEmptyRendererBitmap());
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
  ASSERT_THAT(app.selectedElements(), SizeIs(1u));
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
        if (!renderTarget.texture) {
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
  ASSERT_THAT(actual, NonEmptyRendererBitmap());
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
  settle();
  ASSERT_FALSE(penTool.isDrafting());
  ASSERT_THAT(app.selectedElements(), SizeIs(1u));
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
        if (!renderTarget.texture) {
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
  ASSERT_THAT(actual, NonEmptyRendererBitmap());
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
  const auto* geodeTexture = static_cast<const svg::RendererGeodeTextureSnapshot*>(texture.get());
  const WGPUTextureView textureView = geodeTexture->textureView();
  const ImTextureID textureId =
      static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(textureView));
  ImGui_ImplWGPU_AddTexturePremultipliedAlphaRef(textureId);

  window.beginFrame();
  ImGui::GetBackgroundDrawList()->AddImage(textureId, ImVec2(16.0f, 16.0f), ImVec2(80.0f, 80.0f));
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();

  ASSERT_THAT(actual, NonEmptyRendererBitmap());
  const std::array<std::uint8_t, 4> center = PixelAt(actual, 48, 48);
  EXPECT_THAT(center, Rgba(Near(128, 3), testing::Le(3), testing::Le(3), testing::Eq(255)))
      << "A premultiplied red texture should not be multiplied by alpha again during ImGui "
         "presentation.";
  ImGui_ImplWGPU_RemoveTexturePremultipliedAlphaRef(textureId);
  ImGui_ImplWGPU_RemoveTexture(textureId);
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
  ASSERT_THAT(textures.tiles(), SizeIs(1u));
  ASSERT_NE(textures.tiles().front().texture, 0);

  window.beginFrame();
  ImGui::GetBackgroundDrawList()->AddImage(textures.tiles().front().texture, ImVec2(16.0f, 16.0f),
                                           ImVec2(48.0f, 48.0f));
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();

  ASSERT_THAT(actual, NonEmptyRendererBitmap());
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

  ASSERT_THAT(actual, NonEmptyRendererBitmap());
  const std::array<std::uint8_t, 4> center = PixelAt(actual, 37, 28);
  EXPECT_THAT(center, Rgba(testing::Le(8), testing::Ge(245), testing::Le(8), testing::Eq(255)))
      << "The middle band should stay green; sampling the full power-of-two texture instead of "
         "the payload UV shifts the blue edge padding into the center.";
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
  const svg::RendererBitmap* panelThumbnail = panel.rowThumbnail(row->stableId);
  ASSERT_NE(panelThumbnail, nullptr);
  EXPECT_EQ(panelThumbnail->dimensions, goldenBitmap->dimensions)
      << "The UI presentation test isolates ImGui/WGPU by uploading the approved golden, but the "
         "row layout must still reserve the approved thumbnail size.";

  GlTextureCache textures(window.geodeDevice());
  textures.initialize();
  GlTextureCache::ThumbnailTextureView actualUpload;
  const LayersPanel::ThumbnailTextureProvider textureProvider =
      [targetStableId = row->stableId, &textures, &goldenBitmap, &actualUpload](
          std::uint64_t stableId, const svg::RendererBitmap&) -> LayersPanel::ThumbnailTexture {
    if (stableId != targetStableId) {
      return LayersPanel::ThumbnailTexture{};
    }

    actualUpload = textures.uploadThumbnail(/*key=*/0xbac65001u, *goldenBitmap);
    return LayersPanel::ThumbnailTexture{
        .texture = actualUpload.texture,
        .uvBottomRight = actualUpload.uvBottomRight,
    };
  };
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
  panel.render(&app, textureProvider);
  const std::optional<ImageDrawRect> actualRect =
      FindTextureDrawRect(*ImGui::GetWindowDrawList(), actualUpload.texture);
  ImGui::End();
  const svg::RendererBitmap actualFramebuffer = window.endFrameAndReadPixels();
  ASSERT_THAT(actualFramebuffer, NonEmptyRendererBitmap());
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
  ASSERT_THAT(expectedFramebuffer, NonEmptyRendererBitmap());

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
  ASSERT_THAT(liveOwner.tiles(), SizeIs(1u));
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

  ASSERT_THAT(actual, NonEmptyRendererBitmap());
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
  const ImTextureID textureId = TextureIdFromGeodeSnapshot(*texture);
  ASSERT_NE(textureId, 0);
  ImGui_ImplWGPU_AddTexturePremultipliedAlphaRef(textureId);

  window.beginFrame();
  ImDrawList* drawList = ImGui::GetBackgroundDrawList();
  drawList->AddImage(textureId, ImVec2(16.0f, 68.0f), ImVec2(112.0f, 164.0f));
  drawList->AddImage(textureId, ImVec2(144.0f, 20.0f), ImVec2(336.0f, 212.0f));
  const svg::RendererBitmap actual = window.endFrameAndReadPixels();
  WriteDiagnosticBitmap(actual, "zoomed_blurred_premul_presentation.png");

  ASSERT_THAT(actual, NonEmptyRendererBitmap());
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

  ASSERT_THAT(actual, NonEmptyRendererBitmap());
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

  ASSERT_THAT(actual, NonEmptyRendererBitmap());
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
