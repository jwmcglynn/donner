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
    std::optional<SelectTool::ActiveDragPreview> displayedPreview = std::nullopt,
    bool documentPresentedDirectly = false, bool compositorTileOverlay = false) {
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
      .documentPresentedDirectly = documentPresentedDirectly,
      .compositorTileOverlay = compositorTileOverlay,
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

TEST(RenderPanePresenterVisualReproTest, CompositorTileOverlayRendersDuringDirectPresentation) {
  gui::EditorWindow window(gui::EditorWindowOptions{
      .title = "Direct Presentation Compositor Tile Overlay Repro",
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

  RenderResult::CompositedPreview preview;
  preview.tiles.push_back(MakeTile(
      "layer:7", RenderResult::CompositedTile::Kind::Layer, kVisibleDragEntity,
      Vector2d(54.0, 44.0), Vector2d(78.0, 78.0), kVisibleDragLayer, /*isDragTarget=*/false));

  GlTextureCache textures(window.geodeDevice());
  textures.initialize();
  textures.uploadComposited(preview);

  const svg::RendererBitmap withoutOverlay =
      CapturePresenterFrame(&window, &textures, entt::null, std::nullopt, std::nullopt,
                            /*documentPresentedDirectly=*/true,
                            /*compositorTileOverlay=*/false);
  const svg::RendererBitmap withOverlay =
      CapturePresenterFrame(&window, &textures, entt::null, std::nullopt, std::nullopt,
                            /*documentPresentedDirectly=*/true,
                            /*compositorTileOverlay=*/true);

  ASSERT_FALSE(withoutOverlay.empty());
  ASSERT_FALSE(withOverlay.empty());
  int changedPixels = 0;
  for (std::size_t offset = 0; offset + 3u < withOverlay.pixels.size(); offset += 4u) {
    if (withOverlay.pixels[offset + 0u] != withoutOverlay.pixels[offset + 0u] ||
        withOverlay.pixels[offset + 1u] != withoutOverlay.pixels[offset + 1u] ||
        withOverlay.pixels[offset + 2u] != withoutOverlay.pixels[offset + 2u] ||
        withOverlay.pixels[offset + 3u] != withoutOverlay.pixels[offset + 3u]) {
      ++changedPixels;
    }
  }
  EXPECT_GT(changedPixels, 0)
      << "The compositor tile overlay must remain visible when document pixels are presented "
         "directly to the framebuffer.";
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

// --- Performance overlay (FPS pill and full frame/memory graph) ---

gui::EditorWindowOptions PerfOverlayWindowOptions(const char* title) {
  return gui::EditorWindowOptions{
      .title = title,
      .initialWidth = kLogicalWidth,
      .initialHeight = kLogicalHeight,
      .visible = false,
      .offscreen = true,
      .offscreenContentScale = 1.0,
      .clearColor = {0.08f, 0.09f, 0.10f, 1.0f},
      .enableFramebufferReadback = true,
  };
}

/// Render one presenter frame with an explicit frame history and perf overlay
/// mode, returning the readback pixels.
svg::RendererBitmap CapturePerfOverlayFrame(gui::EditorWindow* window, GlTextureCache* textures,
                                            const FrameHistory& frameHistory,
                                            PerfOverlayMode perfOverlayMode) {
  ViewportState viewport;
  viewport.paneOrigin = Vector2d(0.0, 0.0);
  viewport.paneSize = Vector2d(kLogicalWidth, kLogicalHeight);
  viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, static_cast<double>(kLogicalWidth),
                                             static_cast<double>(kLogicalHeight));
  viewport.devicePixelRatio = 1.0;
  viewport.zoom = 1.0;
  viewport.panDocPoint = Vector2d(0.0, 0.0);
  viewport.panScreenPoint = Vector2d(0.0, 0.0);

  const std::optional<SelectionChromeSnapshot> noOverlaySnapshot;
  const std::optional<SelectTool::ActiveDragPreview> noPreview;
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
  ImGui::Begin("perf-overlay-repro", nullptr, kWindowFlags);
  presenter.render(RenderPanePresenterState{
      .viewport = viewport,
      .frameHistory = frameHistory,
      .textures = *textures,
      .immediateOverlaySnapshot = noOverlaySnapshot,
      .activeDragPreview = noPreview,
      .displayedDragPreview = noPreview,
      .contentRegion = Vector2d(kLogicalWidth, kLogicalHeight),
      // Match the editor's direct-presentation path: without it, an empty tile
      // set early-returns from render() before the perf overlay is drawn.
      .documentPresentedDirectly = true,
      .perfOverlayMode = perfOverlayMode,
  });
  ImGui::End();
  ImGui::PopStyleVar();
  return window->endFrameAndReadPixels();
}

/// Count pixels in a logical-coordinate rect for which @p predicate holds.
template <typename Predicate>
int CountPixelsWhere(const svg::RendererBitmap& bitmap, int left, int top, int right, int bottom,
                     Predicate predicate) {
  if (bitmap.empty()) {
    return 0;
  }
  int count = 0;
  for (int logicalY = top; logicalY < bottom; ++logicalY) {
    for (int logicalX = left; logicalX < right; ++logicalX) {
      const std::array<std::uint8_t, 4> px = PixelAtLogical(bitmap, logicalX, logicalY);
      if (predicate(px[0], px[1], px[2])) {
        ++count;
      }
    }
  }
  return count;
}

bool IsBudgetMissRed(int r, int g, int b) {
  return r > 180 && g < 90 && b < 90;
}

bool IsBackendCyan(int r, int g, int b) {
  return r < 80 && g > 150 && b > 170;
}

bool IsRenderBucketBlue(int r, int g, int b) {
  return r < 90 && g > 110 && g < 160 && b > 200;
}

bool IsUiBucketGreen(int r, int g, int b) {
  return r < 60 && g > 130 && b > 90 && b < 140;
}

bool IsHostOrRetiredOrange(int r, int g, int b) {
  return r > 170 && g > 110 && g < 160 && b < 40;
}

bool IsTilesBucketPurple(int r, int g, int b) {
  return r > 120 && r < 170 && g > 110 && g < 160 && b > 200;
}

bool IsBrightReadoutText(int r, int g, int b) {
  (void)g;
  (void)b;
  // Every FrameReadoutColor variant (white, red, orange) has a bright red channel.
  return r > 180;
}

TEST(RenderPanePresenterVisualReproTest, FpsPillDrawsReadoutInBottomRightCorner) {
  gui::EditorWindow window(PerfOverlayWindowOptions("Perf Overlay FPS Pill Repro"));
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }

  GlTextureCache textures(window.geodeDevice());
  textures.initialize();

  FrameHistory history;
  for (int i = 0; i < 30; ++i) {
    history.push(4.0f);
  }

  const svg::RendererBitmap off =
      CapturePerfOverlayFrame(&window, &textures, history, PerfOverlayMode::Off);
  const svg::RendererBitmap pill =
      CapturePerfOverlayFrame(&window, &textures, history, PerfOverlayMode::FpsPill);
  WriteDiagnosticBitmap(pill, "perf_overlay_fps_pill.png");
  ASSERT_FALSE(off.empty());
  ASSERT_FALSE(pill.empty());

  // The pill anchors to the bottom-right corner and draws a bright FPS/ms
  // readout; with the overlay off the same corner stays dark.
  const int pillText = CountPixelsWhere(pill, kLogicalWidth - 120, kLogicalHeight - 35,
                                        kLogicalWidth, kLogicalHeight, IsBrightReadoutText);
  const int offText = CountPixelsWhere(off, kLogicalWidth - 120, kLogicalHeight - 35, kLogicalWidth,
                                       kLogicalHeight, IsBrightReadoutText);
  EXPECT_GT(pillText, 20) << "FPS pill mode should draw a bright readout in the corner.";
  EXPECT_EQ(offText, 0) << "Off mode must not draw any perf overlay.";
}

TEST(RenderPanePresenterVisualReproTest, FullGraphMarksBudgetMissesAndWorkerSamples) {
  gui::EditorWindow window(PerfOverlayWindowOptions("Perf Overlay Frame Graph Repro"));
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }

  GlTextureCache textures(window.geodeDevice());
  textures.initialize();

  // Fast frames: all under the 120 Hz budget, no worker samples.
  FrameHistory fastHistory;
  for (int i = 0; i < 60; ++i) {
    fastHistory.push(4.0f);
  }

  // Slow frames: over the 60 Hz budget with profiled buckets, and worker
  // samples both in a consecutive run (line segments) and isolated (dot).
  FrameHistory slowHistory;
  for (int i = 0; i < 60; ++i) {
    slowHistory.push(20.0f);
    FrameCostBreakdown cost;
    cost.mainFrame.renderPaneMs = 5.0;
    cost.mainFrame.layoutMs = 4.0;
    cost.hostFrame.beginFrameMs = 3.0;
    slowHistory.setLatestFrameCost(cost);
    if (i >= 20 && i < 40) {
      // Consecutive run: connected line. 25 ms places the line in the clear
      // region above the stacked bars so it is not diluted by them.
      slowHistory.setLatestBackendMs(25.0f);
    } else if (i == 50) {
      slowHistory.setLatestBackendMs(25.0f);  // Isolated sample: single dot.
    }
  }

  const svg::RendererBitmap fast =
      CapturePerfOverlayFrame(&window, &textures, fastHistory, PerfOverlayMode::FullGraph);
  const svg::RendererBitmap slow =
      CapturePerfOverlayFrame(&window, &textures, slowHistory, PerfOverlayMode::FullGraph);
  WriteDiagnosticBitmap(slow, "perf_overlay_full_graph_budget_miss.png");
  ASSERT_FALSE(fast.empty());
  ASSERT_FALSE(slow.empty());

  // Budget-miss markers are drawn only above bars that exceed the 60 Hz budget.
  const int slowMissPixels =
      CountPixelsWhere(slow, 0, 0, kLogicalWidth, kLogicalHeight, IsBudgetMissRed);
  const int fastMissPixels =
      CountPixelsWhere(fast, 0, 0, kLogicalWidth, kLogicalHeight, IsBudgetMissRed);
  EXPECT_GT(slowMissPixels, 40) << "Over-budget frames must draw red budget-miss markers.";
  EXPECT_LT(fastMissPixels, slowMissPixels / 4)
      << "Under-budget frames must not draw budget-miss markers.";

  // The worker overlay draws cyan only where backend samples landed.
  const int slowBackendPixels =
      CountPixelsWhere(slow, 0, 0, kLogicalWidth, kLogicalHeight, IsBackendCyan);
  const int fastBackendPixels =
      CountPixelsWhere(fast, 0, 0, kLogicalWidth, kLogicalHeight, IsBackendCyan);
  EXPECT_GT(slowBackendPixels, 10) << "Non-zero worker samples must draw the cyan overlay.";
  EXPECT_EQ(fastBackendPixels, 0) << "Frames without worker samples must not draw the overlay.";

  // Profiled frame-cost buckets stack as distinct colors inside the graph.
  EXPECT_GT(CountPixelsWhere(slow, 0, 0, kLogicalWidth, kLogicalHeight, IsRenderBucketBlue), 20)
      << "The main-render bucket should be visible in the stacked graph.";
  EXPECT_GT(CountPixelsWhere(slow, 0, 0, kLogicalWidth, kLogicalHeight, IsUiBucketGreen), 20)
      << "The UI bucket should be visible in the stacked graph.";
  EXPECT_GT(CountPixelsWhere(slow, 0, 0, kLogicalWidth, kLogicalHeight, IsHostOrRetiredOrange), 20)
      << "The host bucket should be visible in the stacked graph.";
}

TEST(RenderPanePresenterVisualReproTest, FullGraphStacksMemoryBucketsWithPeakLine) {
  gui::EditorWindow window(PerfOverlayWindowOptions("Perf Overlay Memory Graph Repro"));
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }

  GlTextureCache textures(window.geodeDevice());
  textures.initialize();

  constexpr std::uint64_t kMiB = 1024u * 1024u;
  FrameHistory history;
  for (int i = 0; i < 60; ++i) {
    history.push(4.0f);
    FrameMemorySample memory;
    memory.activeTileBytes = 100u * kMiB;
    memory.overviewTileBytes = 20u * kMiB;
    memory.retiredBytes = 50u * kMiB;
    memory.totalTrackedBytes = 220u * kMiB;  // 50 MiB of untracked-bucket overhead.
    memory.peakTrackedBytes = 240u * kMiB;
    history.setLatestMemorySample(memory);
  }

  const svg::RendererBitmap graph =
      CapturePerfOverlayFrame(&window, &textures, history, PerfOverlayMode::FullGraph);
  WriteDiagnosticBitmap(graph, "perf_overlay_memory_graph.png");
  ASSERT_FALSE(graph.empty());

  // Tile bytes stack in purple; retired bytes in orange. The frame graph above
  // has no profiled buckets here (frames carry no cost breakdown), so any
  // orange comes from the memory graph's retired segment.
  EXPECT_GT(CountPixelsWhere(graph, 0, 0, kLogicalWidth, kLogicalHeight, IsTilesBucketPurple), 40)
      << "Active plus overview tile bytes should render as the purple memory bucket.";
  EXPECT_GT(CountPixelsWhere(graph, 0, 0, kLogicalWidth, kLogicalHeight, IsHostOrRetiredOrange), 20)
      << "Retired texture bytes should render as the orange memory bucket.";
}

}  // namespace
}  // namespace donner::editor
