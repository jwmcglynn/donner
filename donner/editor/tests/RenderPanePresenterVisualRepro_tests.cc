#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "donner/base/tests/TestTempDir.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/RenderPanePresenter.h"
#include "donner/editor/gui/EditorWindow.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

constexpr int kLogicalWidth = 320;
constexpr int kLogicalHeight = 220;
constexpr Entity kHiddenLayerEntity = static_cast<Entity>(42);
constexpr Entity kVisibleDragEntity = static_cast<Entity>(7);
constexpr std::array<std::uint8_t, 4> kBackground = {236, 239, 242, 255};
constexpr std::array<std::uint8_t, 4> kHiddenStaleLayer = {224, 67, 57, 255};
constexpr std::array<std::uint8_t, 4> kVisibleDragLayer = {33, 150, 243, 255};
constexpr std::array<std::uint8_t, 4> kDimHiddenOverlay = {95, 154, 178, 255};
constexpr std::array<std::uint8_t, 4> kSelectedOverlay = {25, 135, 224, 255};

svg::RendererBitmap MakeBitmap(int width, int height, std::array<std::uint8_t, 4> rgba) {
  svg::RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(width, height);
  bitmap.rowBytes = static_cast<std::size_t>(width) * 4u;
  bitmap.alphaType = svg::AlphaType::Unpremultiplied;
  bitmap.pixels.resize(bitmap.rowBytes * static_cast<std::size_t>(height));
  for (std::size_t offset = 0; offset + 3u < bitmap.pixels.size(); offset += 4u) {
    bitmap.pixels[offset + 0u] = rgba[0];
    bitmap.pixels[offset + 1u] = rgba[1];
    bitmap.pixels[offset + 2u] = rgba[2];
    bitmap.pixels[offset + 3u] = rgba[3];
  }
  return bitmap;
}

void SetPixel(svg::RendererBitmap* bitmap, int x, int y, std::array<std::uint8_t, 4> rgba) {
  if (x < 0 || y < 0 || x >= bitmap->dimensions.x || y >= bitmap->dimensions.y) {
    return;
  }

  const std::size_t offset =
      static_cast<std::size_t>(y) * bitmap->rowBytes + static_cast<std::size_t>(x) * 4u;
  bitmap->pixels[offset + 0u] = rgba[0];
  bitmap->pixels[offset + 1u] = rgba[1];
  bitmap->pixels[offset + 2u] = rgba[2];
  bitmap->pixels[offset + 3u] = rgba[3];
}

void DrawRectOutline(svg::RendererBitmap* bitmap, int x, int y, int width, int height,
                     int strokeWidth, std::array<std::uint8_t, 4> rgba) {
  for (int inset = 0; inset < strokeWidth; ++inset) {
    for (int px = x + inset; px < x + width - inset; ++px) {
      SetPixel(bitmap, px, y + inset, rgba);
      SetPixel(bitmap, px, y + height - 1 - inset, rgba);
    }
    for (int py = y + inset; py < y + height - inset; ++py) {
      SetPixel(bitmap, x + inset, py, rgba);
      SetPixel(bitmap, x + width - 1 - inset, py, rgba);
    }
  }
}

RenderResult::CompositedTile MakeTile(std::string id, RenderResult::CompositedTile::Kind kind,
                                      Entity layerEntity, Vector2d offset, Vector2d size,
                                      std::array<std::uint8_t, 4> rgba, bool isDragTarget) {
  RenderResult::CompositedTile tile;
  tile.kind = kind;
  tile.id = std::move(id);
  tile.layerEntity = layerEntity;
  tile.generation = 1;
  tile.bitmap = MakeBitmap(static_cast<int>(size.x), static_cast<int>(size.y), rgba);
  tile.bitmapDimsPx = tile.bitmap.dimensions;
  tile.rasterCanvasSize = Vector2i(kLogicalWidth, kLogicalHeight);
  tile.canvasOffsetDoc = offset;
  tile.bitmapDimsDoc = size;
  tile.isDragTarget = isDragTarget;
  return tile;
}

RenderResult::CompositedPreview MakePreview(bool includeHiddenLayer) {
  RenderResult::CompositedPreview preview;
  preview.entity = kVisibleDragEntity;
  preview.tiles.push_back(MakeTile("seg:0", RenderResult::CompositedTile::Kind::Segment, entt::null,
                                   Vector2d(0.0, 0.0), Vector2d(kLogicalWidth, kLogicalHeight),
                                   kBackground, false));

  if (includeHiddenLayer) {
    preview.tiles.push_back(MakeTile("layer:42", RenderResult::CompositedTile::Kind::Layer,
                                     kHiddenLayerEntity, Vector2d(54.0, 44.0), Vector2d(78.0, 78.0),
                                     kHiddenStaleLayer, false));
  }

  preview.tiles.push_back(MakeTile("layer:7", RenderResult::CompositedTile::Kind::Layer,
                                   kVisibleDragEntity, Vector2d(202.0, 44.0), Vector2d(78.0, 78.0),
                                   kVisibleDragLayer, true));
  return preview;
}

svg::RendererBitmap MakeOverlayBitmap() {
  svg::RendererBitmap overlay = MakeBitmap(kLogicalWidth, kLogicalHeight, {0, 0, 0, 0});
  DrawRectOutline(&overlay, 54, 44, 78, 78, 3, kDimHiddenOverlay);
  DrawRectOutline(&overlay, 202, 44, 78, 78, 3, kSelectedOverlay);
  return overlay;
}

std::filesystem::path StableOutputDir() {
  // A fixed, easy-to-open location for the diagnostic screenshots. Prefer the
  // per-test scratch dir (TestTempDir): a fixed directory name in the shared
  // system temp collides across users on remote-execution workers — a
  // directory created by one worker user is unwritable for the next. The
  // canonical CI artifacts still land in $TEST_UNDECLARED_OUTPUTS_DIR below.
  return TestTempDir() / "donner-display-none-ui-repro";
}

void WriteBitmap(const svg::RendererBitmap& bitmap, const std::filesystem::path& outputPath) {
  if (bitmap.empty()) {
    return;
  }

  std::error_code error;
  std::filesystem::create_directories(outputPath.parent_path(), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(svg::RendererImageIO::writeRgbaPixelsToPngFile(
      outputPath.string().c_str(), bitmap.pixels, bitmap.dimensions.x, bitmap.dimensions.y,
      bitmap.rowBytes / 4u));
}

void WriteDiagnosticBitmap(const svg::RendererBitmap& bitmap, std::string_view filename) {
  WriteBitmap(bitmap, StableOutputDir() / filename);
  const char* undeclaredOutputsDir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  if (undeclaredOutputsDir != nullptr) {
    WriteBitmap(bitmap, std::filesystem::path(undeclaredOutputsDir) / filename);
  }
}

svg::RendererBitmap CapturePresenterFrame(
    gui::EditorWindow* window, GlTextureCache* textures, Entity suppressedLayerEntity,
    std::optional<SelectionChromeSnapshot> immediateOverlaySnapshot = std::nullopt) {
  ViewportState viewport;
  viewport.paneOrigin = Vector2d(0.0, 0.0);
  viewport.paneSize = Vector2d(kLogicalWidth, kLogicalHeight);
  viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, static_cast<double>(kLogicalWidth),
                                             static_cast<double>(kLogicalHeight));
  viewport.devicePixelRatio = 1.0;
  viewport.zoom = 1.0;
  viewport.panDocPoint = Vector2d(0.0, 0.0);
  viewport.panScreenPoint = Vector2d(0.0, 0.0);

  FrameHistory frameHistory;
  frameHistory.push(1000.0f / 60.0f);
  const std::optional<SelectTool::ActiveDragPreview> noDragPreview;
  RenderPanePresenter presenter;

  window->beginFrame();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(
      ImVec2(static_cast<float>(kLogicalWidth), static_cast<float>(kLogicalHeight)),
      ImGuiCond_Always);
  constexpr ImGuiWindowFlags kWindowFlags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;
  ImGui::Begin("display-none-suppression-repro", nullptr, kWindowFlags);
  (void)presenter.render(RenderPanePresenterState{
      .viewport = viewport,
      .frameHistory = frameHistory,
      .textures = *textures,
      .immediateOverlaySnapshot = immediateOverlaySnapshot,
      .activeDragPreview = noDragPreview,
      .displayedDragPreview = noDragPreview,
      .contentRegion = Vector2d(kLogicalWidth, kLogicalHeight),
      .suppressedLayerEntity = suppressedLayerEntity,
  });
  ImGui::End();
  ImGui::PopStyleVar();
  return window->endFrameAndReadPixels();
}

int CountVisibleDragPixels(const svg::RendererBitmap& bitmap) {
  if (bitmap.empty()) {
    return 0;
  }

  const double readbackFromLogicalX =
      static_cast<double>(bitmap.dimensions.x) / static_cast<double>(kLogicalWidth);
  const double readbackFromLogicalY =
      static_cast<double>(bitmap.dimensions.y) / static_cast<double>(kLogicalHeight);
  int count = 0;
  for (int logicalY = 50; logicalY < 116; ++logicalY) {
    const int y =
        static_cast<int>(std::lround(static_cast<double>(logicalY) * readbackFromLogicalY));
    for (int logicalX = 208; logicalX < 274; ++logicalX) {
      const int x =
          static_cast<int>(std::lround(static_cast<double>(logicalX) * readbackFromLogicalX));
      const std::size_t offset =
          static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
      const int red = bitmap.pixels[offset + 0u];
      const int green = bitmap.pixels[offset + 1u];
      const int blue = bitmap.pixels[offset + 2u];
      if (blue > 180 && green > 100 && red < 80) {
        ++count;
      }
    }
  }
  return count;
}

int CountImmediateOverlayPixels(const svg::RendererBitmap& bitmap) {
  if (bitmap.empty()) {
    return 0;
  }

  int count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const std::size_t offset =
          static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
      const int red = bitmap.pixels[offset + 0u];
      const int green = bitmap.pixels[offset + 1u];
      const int blue = bitmap.pixels[offset + 2u];
      if (red < 80 && green > 180 && blue > 180) {
        ++count;
      }
    }
  }
  return count;
}

TEST(RenderPanePresenterVisualReproTest, ImmediateOverlaySnapshotDrawsWithoutOverlayTexture) {
  gui::EditorWindow window(gui::EditorWindowOptions{
      .title = "Immediate Overlay UI Repro",
      .initialWidth = kLogicalWidth,
      .initialHeight = kLogicalHeight,
      .visible = false,
      .offscreen = true,
      .offscreenContentScale = 1.0,
      .clearColor = {0.08f, 0.09f, 0.10f, 1.0f},
      .enableFramebufferReadback = true,
  });
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }

  GlTextureCache textures(window.geodeDevice());
  textures.initialize();
  textures.uploadComposited(MakePreview(/*includeHiddenLayer=*/false));
  ASSERT_EQ(textures.overlayWidth(), 0);
  ASSERT_EQ(textures.overlayHeight(), 0);

  SelectionChromeSnapshot overlaySnapshot;
  overlaySnapshot.aabbsDoc.push_back(Box2d::FromXYWH(72.0, 56.0, 118.0, 88.0));
  overlaySnapshot.selectionStrokeWidthWorld = 2.0;

  const svg::RendererBitmap actual =
      CapturePresenterFrame(&window, &textures, entt::null, overlaySnapshot);
  WriteDiagnosticBitmap(actual, "actual_immediate_overlay_snapshot.png");

  EXPECT_GT(CountImmediateOverlayPixels(actual), 200)
      << "Immediate overlay chrome should be presented directly from the captured snapshot even "
         "when the retained overlay texture cache is empty.";
}

TEST(RenderPanePresenterVisualReproTest, WritesDisplayNoneSuppressionScreenshots) {
  gui::EditorWindow window(gui::EditorWindowOptions{
      .title = "Display None Suppression UI Repro",
      .initialWidth = kLogicalWidth,
      .initialHeight = kLogicalHeight,
      .visible = false,
      .offscreen = true,
      .offscreenContentScale = 1.0,
      .clearColor = {0.08f, 0.09f, 0.10f, 1.0f},
      .enableFramebufferReadback = true,
  });
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }

  const svg::RendererBitmap overlay = MakeOverlayBitmap();

  GlTextureCache actualTextures(window.geodeDevice());
  actualTextures.initialize();
  actualTextures.uploadComposited(MakePreview(/*includeHiddenLayer=*/true));
  actualTextures.uploadOverlay(overlay);
  const svg::RendererBitmap actual =
      CapturePresenterFrame(&window, &actualTextures, kHiddenLayerEntity);
  WriteDiagnosticBitmap(actual, "actual_fixed_drag_target_visible.png");

  GlTextureCache expectedTextures(window.geodeDevice());
  expectedTextures.initialize();
  expectedTextures.uploadComposited(MakePreview(/*includeHiddenLayer=*/false));
  expectedTextures.uploadOverlay(overlay);
  const svg::RendererBitmap expected =
      CapturePresenterFrame(&window, &expectedTextures, entt::null);
  WriteDiagnosticBitmap(expected, "expected_visible_drag_target.png");

  const int actualVisiblePixels = CountVisibleDragPixels(actual);
  const int expectedVisiblePixels = CountVisibleDragPixels(expected);
  EXPECT_GT(actualVisiblePixels, 2500)
      << "The display:none suppression path should not hide a different selected drag target.";
  EXPECT_GT(expectedVisiblePixels, 2500)
      << "The expected screenshot should keep the selected drag target visible while the "
         "display:none layer contributes only overlay chrome.";
}

}  // namespace
}  // namespace donner::editor
