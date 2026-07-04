#include <gmock/gmock.h>

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
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

using svg::test::Rgba;
using ::testing::Eq;
using ::testing::Lt;

constexpr int kLogicalWidth = 320;
constexpr int kLogicalHeight = 220;
constexpr Entity kHiddenLayerEntity = static_cast<Entity>(42);
constexpr Entity kVisibleDragEntity = static_cast<Entity>(7);
constexpr std::array<std::uint8_t, 4> kBackground = {236, 239, 242, 255};
constexpr std::array<std::uint8_t, 4> kHiddenStaleLayer = {224, 67, 57, 255};
constexpr std::array<std::uint8_t, 4> kVisibleDragLayer = {33, 150, 243, 255};

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

EditorRasterViewport RasterViewportForTest(bool viewportBounded) {
  EditorRasterViewport viewport;
  viewport.documentRect = Box2d::FromXYWH(0.0, 0.0, static_cast<double>(kLogicalWidth),
                                          static_cast<double>(kLogicalHeight));
  viewport.outputSizePx = Vector2i(kLogicalWidth, kLogicalHeight);
  viewport.semanticCanvasSizePx = Vector2i(kLogicalWidth, kLogicalHeight);
  viewport.outputFromDocument = Transform2d();
  viewport.viewportBounded = viewportBounded;
  return viewport;
}

std::filesystem::path StableOutputDir() {
  // A fixed, easy-to-open location for the diagnostic screenshots. Prefer the
  // per-test scratch dir (TestTempDir): a fixed directory name in the shared
  // system temp collides across users on remote-execution workers - a
  // directory created by one worker user is unwritable for the next. The
  // canonical CI artifacts still land in $TEST_UNDECLARED_OUTPUTS_DIR below.
  return TestTempDir() / "donner-display-none-ui-repro";
}

// Write a diagnostic PNG, returning false if the destination could not be
// written. Used best-effort for the developer-convenience temp-dir copy (whose
// shared path may be unwritable under a sandboxed/remote test runner) and as a
// hard requirement for the canonical $TEST_UNDECLARED_OUTPUTS_DIR artifact.
bool WriteBitmap(const svg::RendererBitmap& bitmap, const std::filesystem::path& outputPath) {
  if (bitmap.empty()) {
    return false;
  }

  std::error_code error;
  std::filesystem::create_directories(outputPath.parent_path(), error);
  if (error) {
    return false;
  }
  return svg::RendererImageIO::writeRgbaPixelsToPngFile(outputPath.string().c_str(), bitmap.pixels,
                                                        bitmap.dimensions.x, bitmap.dimensions.y,
                                                        bitmap.rowBytes / 4u);
}

void WriteDiagnosticBitmap(const svg::RendererBitmap& bitmap, std::string_view filename) {
  // Best-effort developer-convenience copy: a shared temp dir may be
  // unwritable under a sandboxed/remote test runner, which must not fail the
  // test (the assertions below carry the real verification).
  (void)WriteBitmap(bitmap, StableOutputDir() / filename);
  const char* undeclaredOutputsDir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  if (undeclaredOutputsDir != nullptr) {
    EXPECT_TRUE(WriteBitmap(bitmap, std::filesystem::path(undeclaredOutputsDir) / filename));
  }
}

svg::RendererBitmap CapturePresenterFrame(
    gui::EditorWindow* window, GlTextureCache* textures, Entity suppressedLayerEntity,
    std::optional<SelectTool::ActiveDragPreview> activePreview = std::nullopt,
    std::optional<SelectTool::ActiveDragPreview> displayedPreview = std::nullopt) {
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
  // Selection chrome is rendered by OverlayRenderer onto the framebuffer, not by
  // the presenter; the presenter no longer consumes a chrome snapshot.
  const std::optional<SelectionChromeSnapshot> noOverlaySnapshot;
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
  presenter.render(RenderPanePresenterState{
      .viewport = viewport,
      .frameHistory = frameHistory,
      .textures = *textures,
      .immediateOverlaySnapshot = noOverlaySnapshot,
      .activeDragPreview = activePreview,
      .displayedDragPreview = displayedPreview,
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

std::array<std::uint8_t, 4> PixelAtLogical(const svg::RendererBitmap& bitmap, int logicalX,
                                           int logicalY) {
  const double readbackFromLogicalX =
      static_cast<double>(bitmap.dimensions.x) / static_cast<double>(kLogicalWidth);
  const double readbackFromLogicalY =
      static_cast<double>(bitmap.dimensions.y) / static_cast<double>(kLogicalHeight);
  const int x = static_cast<int>(std::lround(static_cast<double>(logicalX) * readbackFromLogicalX));
  const int y = static_cast<int>(std::lround(static_cast<double>(logicalY) * readbackFromLogicalY));
  const std::size_t offset =
      static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
  return {bitmap.pixels[offset + 0u], bitmap.pixels[offset + 1u], bitmap.pixels[offset + 2u],
          bitmap.pixels[offset + 3u]};
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

  // Selection chrome is no longer drawn by the presenter (it is rendered by
  // Donner's OverlayRenderer straight onto the Geode framebuffer), so this
  // exercises the presenter's `display:none` drag-target tile suppression in
  // isolation: the hidden stale layer's tile must not occlude a different
  // selected drag target's tile.
  GlTextureCache actualTextures(window.geodeDevice());
  actualTextures.initialize();
  actualTextures.uploadComposited(MakePreview(/*includeHiddenLayer=*/true));
  const svg::RendererBitmap actual =
      CapturePresenterFrame(&window, &actualTextures, kHiddenLayerEntity);
  WriteDiagnosticBitmap(actual, "actual_fixed_drag_target_visible.png");

  GlTextureCache expectedTextures(window.geodeDevice());
  expectedTextures.initialize();
  expectedTextures.uploadComposited(MakePreview(/*includeHiddenLayer=*/false));
  const svg::RendererBitmap expected =
      CapturePresenterFrame(&window, &expectedTextures, entt::null);
  WriteDiagnosticBitmap(expected, "expected_visible_drag_target.png");

  const int actualVisiblePixels = CountVisibleDragPixels(actual);
  const int expectedVisiblePixels = CountVisibleDragPixels(expected);
  EXPECT_GT(actualVisiblePixels, 2500)
      << "The display:none suppression path should not hide a different selected drag target.";
  EXPECT_GT(expectedVisiblePixels, 2500)
      << "The expected screenshot should keep the selected drag target visible while the "
         "display:none layer is suppressed.";
}

TEST(RenderPanePresenterVisualReproTest, OverviewInfillDoesNotBleedThroughTransparentActiveTile) {
  gui::EditorWindow window(gui::EditorWindowOptions{
      .title = "Transparent Active Tile Overview Infill Repro",
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

  RenderResult::CompositedPreview staleOverview;
  staleOverview.tiles.push_back(MakeTile(
      "full-canvas", RenderResult::CompositedTile::Kind::Segment, entt::null, Vector2d(0.0, 0.0),
      Vector2d(kLogicalWidth, kLogicalHeight), kHiddenStaleLayer, /*isDragTarget=*/false));

  RenderResult::CompositedPreview transparentActive;
  transparentActive.tiles.push_back(MakeTile(
      "full-canvas", RenderResult::CompositedTile::Kind::Segment, entt::null, Vector2d(0.0, 0.0),
      Vector2d(kLogicalWidth, kLogicalHeight), {0, 0, 0, 0}, /*isDragTarget=*/false));

  GlTextureCache textures(window.geodeDevice());
  textures.initialize();
  textures.uploadCompositedOverview(staleOverview,
                                    RasterViewportForTest(/*viewportBounded=*/false));
  textures.uploadComposited(transparentActive, RasterViewportForTest(/*viewportBounded=*/true));
  ASSERT_TRUE(textures.activeTilesViewportBounded());
  ASSERT_FALSE(textures.overviewTiles().empty());

  const svg::RendererBitmap actual = CapturePresenterFrame(&window, &textures, entt::null);
  WriteDiagnosticBitmap(actual, "actual_transparent_active_overview_infill.png");

  ASSERT_FALSE(actual.empty());
  const std::array<std::uint8_t, 4> center =
      PixelAtLogical(actual, kLogicalWidth / 2, kLogicalHeight / 2);
  EXPECT_THAT(center, Rgba(Lt(100), Lt(100), Lt(100), Eq(255)))
      << "A transparent current tile must reveal the checkerboard, not stale red overview pixels.";
}

TEST(RenderPanePresenterVisualReproTest, OverviewInfillDoesNotBleedThroughOldDragTargetBounds) {
  gui::EditorWindow window(gui::EditorWindowOptions{
      .title = "Drag Target Overview Infill Repro",
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

  RenderResult::CompositedPreview staleOverview;
  staleOverview.tiles.push_back(MakeTile(
      "full-canvas", RenderResult::CompositedTile::Kind::Segment, entt::null, Vector2d(0.0, 0.0),
      Vector2d(kLogicalWidth, kLogicalHeight), kHiddenStaleLayer, /*isDragTarget=*/false));

  RenderResult::CompositedPreview activeDragTile;
  activeDragTile.tiles.push_back(
      MakeTile("layer:7", RenderResult::CompositedTile::Kind::Layer, kVisibleDragEntity,
               Vector2d(0.0, 0.0), Vector2d(kLogicalWidth, kLogicalHeight), kVisibleDragLayer,
               /*isDragTarget=*/true));

  GlTextureCache textures(window.geodeDevice());
  textures.initialize();
  textures.uploadCompositedOverview(staleOverview,
                                    RasterViewportForTest(/*viewportBounded=*/false));
  textures.uploadComposited(activeDragTile, RasterViewportForTest(/*viewportBounded=*/true));
  ASSERT_TRUE(textures.activeTilesViewportBounded());
  ASSERT_FALSE(textures.overviewTiles().empty());

  const SelectTool::ActiveDragPreview activePreview{
      .entity = kVisibleDragEntity,
      .translation = Vector2d(80.0, 0.0),
      .documentFromCachedDocument = Transform2d::Translate(Vector2d(80.0, 0.0)),
      .dragGeneration = 1,
  };
  const SelectTool::ActiveDragPreview displayedPreview{
      .entity = kVisibleDragEntity,
      .translation = Vector2d::Zero(),
      .documentFromCachedDocument = Transform2d(),
      .dragGeneration = 1,
  };

  const svg::RendererBitmap actual =
      CapturePresenterFrame(&window, &textures, entt::null, activePreview, displayedPreview);
  WriteDiagnosticBitmap(actual, "actual_drag_target_old_bounds_overview_infill.png");

  ASSERT_FALSE(actual.empty());
  const std::array<std::uint8_t, 4> oldLeftEdge = PixelAtLogical(actual, 40, kLogicalHeight / 2);
  EXPECT_THAT(oldLeftEdge, Rgba(Lt(100), Lt(100), Lt(100), Eq(255)))
      << "Overview infill must not redraw stale drag-target pixels at the old background "
         "position while the active tile is presented at the live drag position.";
}

}  // namespace
}  // namespace donner::editor
