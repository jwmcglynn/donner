/// @file

#include "donner/editor/repro/GlRnrReplay.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/Vector2.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/repro/ReproFile.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/tests/RendererImageTestUtils.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

struct PixelCrop {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

std::filesystem::path DiagnosticOutputDir() {
  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR"); dir != nullptr) {
    return std::filesystem::path(dir);
  }
  return std::filesystem::temp_directory_path();
}

std::filesystem::path RunfilePath(std::string_view path) {
  const std::filesystem::path direct(path);
  if (std::filesystem::exists(direct)) {
    return direct;
  }

  const char* testSrcDir = std::getenv("TEST_SRCDIR");
  if (testSrcDir == nullptr) {
    return direct;
  }

  if (const char* testWorkspace = std::getenv("TEST_WORKSPACE"); testWorkspace != nullptr) {
    const std::filesystem::path workspacePath =
        std::filesystem::path(testSrcDir) / testWorkspace / direct;
    if (std::filesystem::exists(workspacePath)) {
      return workspacePath;
    }
  }

  const std::filesystem::path bzlmodMainPath = std::filesystem::path(testSrcDir) / "_main" / direct;
  if (std::filesystem::exists(bzlmodMainPath)) {
    return bzlmodMainPath;
  }

  return std::filesystem::path(testSrcDir) / direct;
}

const repro::GlRnrReplayCapture* FindCapture(const repro::GlRnrReplayResult& result,
                                             std::uint64_t frameIndex) {
  for (const repro::GlRnrReplayCapture& capture : result.captures) {
    if (capture.frameIndex == frameIndex) {
      return &capture;
    }
  }
  return nullptr;
}

const repro::GlRnrReplayFrameDiagnostics* FindFrameDiagnostics(
    const repro::GlRnrReplayResult& result, std::uint64_t frameIndex) {
  for (const repro::GlRnrReplayFrameDiagnostics& diagnostics : result.frameDiagnostics) {
    if (diagnostics.frameIndex == frameIndex) {
      return &diagnostics;
    }
  }
  return nullptr;
}

bool CanvasSizeCloseEnoughForReplay(const Vector2i& lhs, const Vector2i& rhs) {
  return std::abs(lhs.x - rhs.x) <= 1 && std::abs(lhs.y - rhs.y) <= 1;
}

bool HasStaleSplitTiles(const repro::GlRnrReplayFrameDiagnostics& frame) {
  if (frame.tiles.empty()) {
    return false;
  }

  if (frame.tiles.size() == 1u && frame.tiles.front().id == "full-canvas") {
    return false;
  }

  for (const repro::GlRnrReplayTileDiagnostics& tile : frame.tiles) {
    if (!CanvasSizeCloseEnoughForReplay(tile.rasterCanvasSize, frame.viewportDesiredCanvas)) {
      return true;
    }
  }
  return false;
}

svg::RendererBitmap BitmapFromImage(const svg::Image& image) {
  svg::RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(image.width, image.height);
  bitmap.rowBytes = image.strideInPixels * 4u;
  bitmap.alphaType = svg::AlphaType::Premultiplied;
  bitmap.pixels = image.data;
  return bitmap;
}

std::optional<svg::RendererBitmap> LoadCaptureBitmap(const repro::GlRnrReplayResult& result,
                                                     std::uint64_t frameIndex) {
  const repro::GlRnrReplayCapture* capture = FindCapture(result, frameIndex);
  if (capture == nullptr) {
    ADD_FAILURE() << "GL replay did not capture frame " << frameIndex;
    return std::nullopt;
  }

  std::optional<svg::Image> image =
      svg::RendererImageTestUtils::readRgbaImageFromPngFile(capture->path.string().c_str());
  if (!image.has_value()) {
    return std::nullopt;
  }
  return BitmapFromImage(*image);
}

svg::RendererBitmap CropBitmap(const svg::RendererBitmap& bitmap, const PixelCrop& crop) {
  const int x = std::clamp(crop.x, 0, bitmap.dimensions.x);
  const int y = std::clamp(crop.y, 0, bitmap.dimensions.y);
  const int width = std::clamp(crop.width, 0, bitmap.dimensions.x - x);
  const int height = std::clamp(crop.height, 0, bitmap.dimensions.y - y);

  svg::RendererBitmap cropped;
  cropped.dimensions = Vector2i(width, height);
  cropped.rowBytes = static_cast<std::size_t>(width) * 4u;
  cropped.alphaType = bitmap.alphaType;
  cropped.pixels.resize(cropped.rowBytes * static_cast<std::size_t>(height));

  for (int row = 0; row < height; ++row) {
    const std::uint8_t* source = bitmap.pixels.data() +
                                 static_cast<std::size_t>(y + row) * bitmap.rowBytes +
                                 static_cast<std::size_t>(x) * 4u;
    std::uint8_t* target = cropped.pixels.data() + static_cast<std::size_t>(row) * cropped.rowBytes;
    std::memcpy(target, source, cropped.rowBytes);
  }

  return cropped;
}

std::optional<double> YellowCentroidY(const svg::RendererBitmap& bitmap) {
  double totalY = 0.0;
  int count = 0;

  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const std::size_t offset =
          static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
      const std::uint8_t red = bitmap.pixels[offset];
      const std::uint8_t green = bitmap.pixels[offset + 1];
      const std::uint8_t blue = bitmap.pixels[offset + 2];
      const std::uint8_t alpha = bitmap.pixels[offset + 3];
      if (alpha > 200 && red > 180 && green > 130 && blue < 90 && red - green < 110) {
        totalY += static_cast<double>(y);
        ++count;
      }
    }
  }

  if (count == 0) {
    return std::nullopt;
  }
  return totalY / static_cast<double>(count);
}

int CountBrightGreenPixels(const svg::RendererBitmap& bitmap) {
  int count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const std::size_t offset =
          static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
      const std::uint8_t red = bitmap.pixels[offset];
      const std::uint8_t green = bitmap.pixels[offset + 1];
      const std::uint8_t blue = bitmap.pixels[offset + 2];
      const std::uint8_t alpha = bitmap.pixels[offset + 3];
      if (alpha > 200 && red < 40 && green > 200 && blue < 40) {
        ++count;
      }
    }
  }
  return count;
}

TEST(GlRnrReplayTest, UsesEmbeddedSvgSourceWhenOriginalPathIsMissing) {
  const std::filesystem::path outputDir = DiagnosticOutputDir();
  const std::filesystem::path reproPath = outputDir / "embedded_svg_source_replay.rnr";

  repro::ReproFile file;
  file.metadata.svgPath = "missing_original_input.svg";
  file.metadata.svgBasename = "embedded_input.svg";
  file.metadata.svgContentHash = "fnv1a64:test";
  file.metadata.svgSource =
      R"(<svg viewBox="0 0 10 10"><rect width="10" height="10" fill="#ffcc00"/></svg>)";
  file.metadata.windowWidth = 640;
  file.metadata.windowHeight = 480;
  file.metadata.displayScale = 1.0;

  repro::ReproFrame frame;
  frame.index = 0;
  frame.deltaMs = 16.667;
  frame.mouseX = 20.0;
  frame.mouseY = 20.0;
  file.frames.push_back(frame);

  ASSERT_TRUE(repro::WriteReproFile(reproPath, file));

  repro::GlRnrReplayOptions options;
  options.rnrPath = reproPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(0);
  options.pace = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;
  ASSERT_NE(FindCapture(result, 0), nullptr);

  std::error_code ec;
  std::filesystem::remove(reproPath, ec);
}

// TODO(#601): Re-enable once the multi-thread determinism test framework lands.
// `pace=true` source-pane character input reparses the document while the async render worker is
// mid-render, so the document is transiently `ThreadingMode::ConcurrentDom`. The editor still
// performs unguarded live-document reads on the UI thread whose timing relative to the worker's
// render window is nondeterministic, so the replay intermittently aborts on
// `ElementAnchor::assertScopedEntityHandleAccessAllowed` (`SVGElement.cc:253`). Eliminating this
// requires completing the editor's ConcurrentDom read-guarding plus deterministic replay, tracked
// by the determinism-framework task (#601).
TEST(GlRnrReplayTest, DISABLED_ReplaysSourcePaneCharacterInput) {
  const std::filesystem::path outputDir = DiagnosticOutputDir();
  const std::filesystem::path reproPath = outputDir / "source_pane_character_input_replay.rnr";
  // The document renders at actual size in the replay (no zoom is applied), so a
  // tiny viewBox would paint fewer pixels than the bright-green threshold below.
  // Use a 200x200 document so the recolored rect dominates the render pane crop.
  constexpr std::string_view kInitialSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200"><rect width="200" height="200" fill="#ff0000"/></svg>)";
  constexpr std::string_view kEditedSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200"><rect width="200" height="200" fill="#00ff00"/></svg>)";

  repro::ReproFile file;
  file.metadata.svgPath = "missing_text_edit_input.svg";
  file.metadata.svgBasename = "text_edit_input.svg";
  file.metadata.svgContentHash = "fnv1a64:test";
  file.metadata.svgSource = std::string(kInitialSource);
  file.metadata.windowWidth = 1400;
  file.metadata.windowHeight = 600;
  file.metadata.displayScale = 1.0;

  const auto pushFrame = [&](std::uint64_t index, double mouseX, double mouseY, int buttons,
                             int modifiers, std::vector<repro::ReproEvent> events = {}) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) * 0.01;
    frame.deltaMs = 10.0;
    frame.mouseX = mouseX;
    frame.mouseY = mouseY;
    frame.mouseButtonMask = buttons;
    frame.modifiers = modifiers;
    frame.events = std::move(events);
    file.frames.push_back(std::move(frame));
  };

  pushFrame(0, 30.0, 70.0, 0, 0);
  repro::ReproEvent mouseDown;
  mouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  mouseDown.mouseButton = 0;
  pushFrame(1, 30.0, 70.0, 1, 0, {mouseDown});
  repro::ReproEvent mouseUp;
  mouseUp.kind = repro::ReproEvent::Kind::MouseUp;
  mouseUp.mouseButton = 0;
  pushFrame(2, 30.0, 70.0, 0, 0, {mouseUp});
  repro::ReproEvent selectAllDown;
  selectAllDown.kind = repro::ReproEvent::Kind::KeyDown;
  selectAllDown.key = static_cast<int>(ImGuiKey_A);
  selectAllDown.modifiers = 1 << 0;
  pushFrame(3, 30.0, 70.0, 0, 1 << 0, {selectAllDown});
  repro::ReproEvent selectAllUp;
  selectAllUp.kind = repro::ReproEvent::Kind::KeyUp;
  selectAllUp.key = static_cast<int>(ImGuiKey_A);
  pushFrame(4, 30.0, 70.0, 0, 0, {selectAllUp});

  std::vector<repro::ReproEvent> characterEvents;
  for (const unsigned char c : kEditedSource) {
    repro::ReproEvent event;
    event.kind = repro::ReproEvent::Kind::Char;
    event.codepoint = c;
    characterEvents.push_back(event);
  }
  pushFrame(5, 30.0, 70.0, 0, 0, std::move(characterEvents));
  for (std::uint64_t index = 6; index <= 60; ++index) {
    pushFrame(index, 30.0, 70.0, 0, 0);
  }

  ASSERT_TRUE(repro::WriteReproFile(reproPath, file));

  repro::GlRnrReplayOptions options;
  options.rnrPath = reproPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(60);
  options.cropMode = repro::GlRnrReplayCropMode::Full;
  options.pace = true;
  options.maxFrame = 60;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;

  std::optional<svg::RendererBitmap> bitmap = LoadCaptureBitmap(result, 60);
  ASSERT_TRUE(bitmap.has_value());
  const svg::RendererBitmap renderPaneCrop =
      CropBitmap(*bitmap, PixelCrop{.x = 560, .y = 0, .width = 500, .height = 600});
  EXPECT_GT(CountBrightGreenPixels(renderPaneCrop), 100);

  std::error_code ec;
  std::filesystem::remove(reproPath, ec);
}

// TODO(#601): Re-enable once the multi-thread determinism test framework lands.
// This `pace=true` recording races the async render worker against the UI thread.
// During a render the document is transiently `ThreadingMode::ConcurrentDom`, and
// the editor still performs unguarded live-document reads on the UI thread
// (selection-chrome geometry via `CollectRenderableGeometry`, and others) whose
// timing relative to the worker's render window is nondeterministic. Depending on
// that timing the replay either aborts on a scoped-access assertion
// (`ElementAnchor::assertScopedEntityHandleAccessAllowed`, an unguarded read
// landing inside a ConcurrentDom window) or serializes the UI against the worker's
// write-held render phases (`prepareDocumentForRendering` / `rasterizeLayer`) for
// the entire gesture — observed flipping between a sub-second abort and a
// multi-minute crawl across runs of the same binary. Making this both fast and
// non-flaky requires completing the editor's ConcurrentDom read-guarding *and* the
// deterministic replay framework, which is tracked by the determinism-framework
// task (#601). Until then this scenario is not a deterministic invariant.
TEST(GlRnrReplayTest, DISABLED_ClickAfterZoomBeforeRerasterSelectsNewTarget) {
  constexpr std::string_view kSourceRnrPath = "donner/editor/tests/drag_start_hang_repro.rnr";
  std::optional<repro::ReproFile> source = repro::ReadReproFile(RunfilePath(kSourceRnrPath));
  ASSERT_TRUE(source.has_value());
  ASSERT_TRUE(source->metadata.expect.has_value());
  const repro::ReproExpectation& expect = *source->metadata.expect;
  ASSERT_EQ(expect.proofKind, repro::ReproExpectationProofKind::WorkerLiveness);
  ASSERT_TRUE(expect.expectedSelectionLabel.has_value());
  ASSERT_TRUE(expect.statusStartFrameIndex.has_value());
  ASSERT_TRUE(expect.statusMaxFrameIndex.has_value());
  ASSERT_TRUE(expect.forbiddenStatusSubstring.has_value());

  repro::ReproFile reproFile = *source;
  const std::uint64_t kClickFrame = static_cast<std::uint64_t>(expect.minFrameIndex);
  const std::uint64_t kLastZoomFrame = kClickFrame - 1;
  reproFile.frames.erase(
      std::remove_if(reproFile.frames.begin(), reproFile.frames.end(),
                     [&](const repro::ReproFrame& frame) { return frame.index > kLastZoomFrame; }),
      reproFile.frames.end());
  ASSERT_FALSE(reproFile.frames.empty());

  const repro::ReproFrame zoomedFrame = reproFile.frames.back();
  ASSERT_TRUE(zoomedFrame.viewport.has_value());

  constexpr double kFrameDtMs = 1000.0 / 60.0;
  for (std::size_t i = 0; i < reproFile.frames.size(); ++i) {
    reproFile.frames[i].index = static_cast<std::uint64_t>(i);
    reproFile.frames[i].timestampSeconds = static_cast<double>(i) / 60.0;
    reproFile.frames[i].deltaMs = kFrameDtMs;
  }

  const std::uint64_t kMouseUpFrame = kClickFrame + 16;
  const std::uint64_t kFinalFrame = static_cast<std::uint64_t>(expect.maxFrameIndex);
  for (std::uint64_t frameIndex = kClickFrame; frameIndex <= kFinalFrame; ++frameIndex) {
    repro::ReproFrame frame = zoomedFrame;
    frame.index = frameIndex;
    frame.timestampSeconds = static_cast<double>(frameIndex) / 60.0;
    frame.deltaMs = kFrameDtMs;
    frame.mouseX = 761.0;
    frame.mouseY = 838.0;
    frame.mouseDocX.reset();
    frame.mouseDocY.reset();
    frame.mouseButtonMask = frameIndex < kMouseUpFrame ? 1 : 0;
    frame.events.clear();
    if (frameIndex == kClickFrame) {
      repro::ReproEvent event;
      event.kind = repro::ReproEvent::Kind::MouseDown;
      event.mouseButton = 0;
      frame.events.push_back(event);
    } else if (frameIndex == kMouseUpFrame) {
      repro::ReproEvent event;
      event.kind = repro::ReproEvent::Kind::MouseUp;
      event.mouseButton = 0;
      frame.events.push_back(event);
    }
    reproFile.frames.push_back(frame);
  }

  const std::filesystem::path reproPath =
      DiagnosticOutputDir() / "zoom_click_before_reraster_selects_new_target.rnr";
  ASSERT_TRUE(repro::WriteReproFile(reproPath, reproFile));

  repro::GlRnrReplayOptions options;
  options.rnrPath = reproPath;
  options.svgPathOverride = RunfilePath("donner_splash.svg");
  options.outputDir = DiagnosticOutputDir() / "gl_zoom_click_before_reraster";
  options.captureFrames = {kFinalFrame};
  options.maxFrame = kFinalFrame;
  options.cropMode = repro::GlRnrReplayCropMode::Full;
  options.pace = true;
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;

  ASSERT_TRUE(result.finalSelectedElementLabel.has_value());
  EXPECT_EQ(*result.finalSelectedElementLabel, *expect.expectedSelectionLabel);

  const repro::GlRnrReplayFrameDiagnostics* firstDiagnostics =
      FindFrameDiagnostics(result, static_cast<std::uint64_t>(*expect.statusStartFrameIndex));
  ASSERT_NE(firstDiagnostics, nullptr);
  const repro::GlRnrReplayFrameDiagnostics* finalDiagnostics =
      FindFrameDiagnostics(result, static_cast<std::uint64_t>(*expect.statusMaxFrameIndex));
  ASSERT_NE(finalDiagnostics, nullptr);
  EXPECT_EQ(finalDiagnostics->statusSuffix.find(*expect.forbiddenStatusSubstring),
            std::string::npos)
      << "stale compositor status persisted through the bounded replay window";
}

// TODO(#601): Re-enable once the multi-thread determinism test framework lands.
// This compares the active-drag frame against the mouse-up frame, but `pace=true`
// lets the async render worker land between the two frames nondeterministically
// (chronically flaky on CI across all branches). Making the replay deterministic
// further showed the residual diff is the *selection chrome* (the active-drag
// transform-preview bounding box vs the settled committed box), which changes
// intentionally after the drag settles — so pixel-identity including chrome is
// not a true invariant. The content "no-jump" invariant for this recording is
// already covered deterministically by
// `RnrReplayTest.FilterPostDragJumpReplayMatchesGroundTruth`.
TEST(GlRnrReplayTest, DISABLED_SecondDragActiveFrameMatchesMouseUpFrame) {
  constexpr std::string_view kRnrPath = "donner/editor/tests/filter_post_drag_jump.rnr";
  const std::filesystem::path rnrPath = RunfilePath(kRnrPath);
  std::optional<repro::ReproFile> reproFile = repro::ReadReproFile(rnrPath);
  ASSERT_TRUE(reproFile.has_value());
  ASSERT_TRUE(reproFile->metadata.expect.has_value());
  const repro::ReproExpectation& expect = *reproFile->metadata.expect;
  ASSERT_EQ(expect.proofKind, repro::ReproExpectationProofKind::ActiveDragAlignment);
  ASSERT_TRUE(expect.activeFrameIndex.has_value());
  ASSERT_TRUE(expect.comparisonFrameIndex.has_value());
  ASSERT_EQ(expect.cropMode, "document-canvas");

  const std::uint64_t activeFrame = static_cast<std::uint64_t>(*expect.activeFrameIndex);
  const std::uint64_t comparisonFrame = static_cast<std::uint64_t>(*expect.comparisonFrameIndex);

  repro::GlRnrReplayOptions options;
  options.rnrPath = rnrPath;
  options.svgPathOverride = RunfilePath("donner_splash.svg");
  options.outputDir = DiagnosticOutputDir() / "gl_second_drag_alignment_repro";
  options.captureFrames = {activeFrame, comparisonFrame};
  options.maxFrame = comparisonFrame;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = true;
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;

  std::optional<svg::RendererBitmap> active = LoadCaptureBitmap(result, activeFrame);
  std::optional<svg::RendererBitmap> comparison = LoadCaptureBitmap(result, comparisonFrame);
  ASSERT_TRUE(active.has_value());
  ASSERT_TRUE(comparison.has_value());

  tests::CompareBitmapToBitmap(*active, *comparison,
                               "gl_second_drag_frame_249_active_vs_250_mouseup",
                               tests::PixelmatchIdentityParams());
}

// TODO(#601): Re-enable once the multi-thread determinism test framework lands.
// The hard precondition `HasStaleSplitTiles(*guarded)` at frame 146 requires the
// async render worker to still be several frames behind on the zoom re-raster — a
// `pace=true` wall-clock race that is chronically flaky on CI (false ~50% of the
// time, including on `main`). The guard invariant under test
// (`ShouldUploadImmediateOverlayForPresentedTiles` rejecting stale split-tile
// epochs) is already covered deterministically by `RenderCoordinatorTest`'s
// `ImmediateOverlayUploadRequiresCurrentSplitCanvasEpoch` /
// `SplitPreviewFromStaleCanvasEpochIsRejected` in AsyncRenderer_tests.cc.
TEST(GlRnrReplayTest, DISABLED_ZoomOutDragDoesNotPublishNewOverlayOverStaleSplitTiles) {
  constexpr std::string_view kRnrPath = "zoom-out-drag-jump.rnr";
  constexpr std::uint64_t kBeforeFrame = 145;
  constexpr std::uint64_t kGuardedFrame = 146;
  // The zoom-out gesture re-rasterizes a 3298x1893 canvas down to 1896x1088 on
  // the async render worker. Re-rasterization only completes well after the
  // gesture ends (drag release is frame 186), so the caught-up frame is found by
  // scanning rather than hard-coded — its exact index depends on worker speed.
  // Replay the full recording so the catch-up is observable.
  constexpr std::uint64_t kLastFrame = 252;

  repro::GlRnrReplayOptions options;
  options.rnrPath = RunfilePath(kRnrPath);
  options.svgPathOverride = RunfilePath("donner_splash.svg");
  options.outputDir = DiagnosticOutputDir() / "gl_zoom_out_drag_overlay_epoch";
  options.captureFrames = {kLastFrame};
  options.maxFrame = kLastFrame;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  // The stale split-tile window only exists while the async render worker is
  // genuinely behind the viewport, and that lag develops only under real-time
  // pacing. With pace=false the worker never lands a render in this headless
  // replay, so no composited tiles are published and the window can't be
  // observed (the stale-tile precondition below would never hold).
  options.pace = true;
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;

  const repro::GlRnrReplayFrameDiagnostics* before = FindFrameDiagnostics(result, kBeforeFrame);
  const repro::GlRnrReplayFrameDiagnostics* guarded = FindFrameDiagnostics(result, kGuardedFrame);
  ASSERT_NE(before, nullptr);
  ASSERT_NE(guarded, nullptr);

  ASSERT_TRUE(HasStaleSplitTiles(*guarded))
      << "The fixture should exercise the stale split-tile zoom window at frame " << kGuardedFrame;
  EXPECT_EQ(guarded->overlayDimsPx, before->overlayDimsPx)
      << "A current-canvas overlay must not publish over stale split tiles.";
  EXPECT_EQ(guarded->overlayTextureHandle, before->overlayTextureHandle)
      << "Frame " << kGuardedFrame
      << " should retain the previous overlay until the split content catches up.";
  EXPECT_FALSE(
      CanvasSizeCloseEnoughForReplay(guarded->overlayDimsPx, guarded->viewportDesiredCanvas))
      << "Publishing a viewport-current overlay over stale split tiles reintroduces the "
         "one-frame texture splat repro.";

  // Once the worker finishes re-rasterizing at the settled viewport, the split
  // tiles catch up and the overlay tracks the viewport canvas again. How many
  // frames that takes — or whether it lands within the recording at all —
  // depends on the async render worker's wall-clock throughput, which varies
  // across machines (a slower CI runner may still be catching up at the final
  // recorded frame). So the catch-up is verified best-effort: when the replay
  // does reach it, confirm the overlay tracks the viewport; never fail the test
  // just because this machine didn't finish catching up in time. The hard
  // guarantee under test is the stale-window invariant asserted above.
  const repro::GlRnrReplayFrameDiagnostics* caughtUp = nullptr;
  for (const repro::GlRnrReplayFrameDiagnostics& frame : result.frameDiagnostics) {
    if (frame.frameIndex > kGuardedFrame && !HasStaleSplitTiles(frame) &&
        CanvasSizeCloseEnoughForReplay(frame.overlayDimsPx, frame.viewportDesiredCanvas)) {
      caughtUp = &frame;
      break;
    }
  }
  if (caughtUp != nullptr) {
    EXPECT_FALSE(HasStaleSplitTiles(*caughtUp));
    EXPECT_TRUE(
        CanvasSizeCloseEnoughForReplay(caughtUp->overlayDimsPx, caughtUp->viewportDesiredCanvas));
  }
}

// TODO(#601): Re-enable once the multi-thread determinism test framework lands. Even with
// `pace=false`, this replay drives the async render worker, so the document is transiently
// `ThreadingMode::ConcurrentDom` while the UI thread performs unguarded live-document reads. Their
// timing relative to the worker's render window is nondeterministic, so the replay intermittently
// aborts on `ElementAnchor::assertScopedEntityHandleAccessAllowed` (`SVGElement.cc:253`). The race
// exists regardless of pacing; eliminating it needs the editor's ConcurrentDom read-guarding plus
// deterministic replay, tracked by the determinism-framework task (#601).
TEST(GlRnrReplayTest, DISABLED_GeodeDragZoomOReplayCoversTextureReuseWindow) {
  constexpr std::string_view kRnrPath = "donner/editor/tests/geode_drag_zoom_o_pop.rnr";
  constexpr std::uint64_t kFirstCaptureFrame = 78;
  constexpr std::uint64_t kLastCaptureFrame = 81;

  const std::filesystem::path rnrPath = RunfilePath(kRnrPath);
  std::optional<repro::ReproFile> reproFile = repro::ReadReproFile(rnrPath);
  ASSERT_TRUE(reproFile.has_value());
  ASSERT_TRUE(reproFile->metadata.expect.has_value());
  const repro::ReproExpectation& expect = *reproFile->metadata.expect;
  ASSERT_EQ(expect.proofKind, repro::ReproExpectationProofKind::PresentedPixels);
  ASSERT_EQ(expect.cropMode, "document-canvas");
  ASSERT_EQ(expect.targetSelector, "#Donner path:nth-of-type(2)");

  repro::GlRnrReplayOptions options;
  options.rnrPath = rnrPath;
  options.svgPathOverride = RunfilePath("donner_splash.svg");
  options.outputDir = DiagnosticOutputDir() / "gl_geode_drag_zoom_o_pop";
  options.captureFrames = {kFirstCaptureFrame, 79, 80, kLastCaptureFrame};
  options.maxFrame = kLastCaptureFrame;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;

  for (std::uint64_t frame = kFirstCaptureFrame; frame <= kLastCaptureFrame; ++frame) {
    const repro::GlRnrReplayFrameDiagnostics* diagnostics = FindFrameDiagnostics(result, frame);
    ASSERT_NE(diagnostics, nullptr) << "missing diagnostics for replay frame " << frame;
    EXPECT_EQ(diagnostics->metadataOnlyMissCount, 0)
        << "metadata-only reuse unexpectedly missed in root-cause replay frame " << frame;
    EXPECT_EQ(diagnostics->duplicateLiveTextureCount, 0)
        << "duplicate live texture handles in root-cause replay frame " << frame;

    std::optional<svg::RendererBitmap> capture = LoadCaptureBitmap(result, frame);
    ASSERT_TRUE(capture.has_value()) << "missing capture for replay frame " << frame;
    EXPECT_GT(capture->dimensions.x, 0);
    EXPECT_GT(capture->dimensions.y, 0);
  }
}

// TODO(#601): Re-enable once the multi-thread determinism test framework lands. `pace=true` drag
// replay races the async render worker against UI-thread input under
// `ThreadingMode::ConcurrentDom`; unguarded UI-thread live-document reads landing inside the
// worker's render window intermittently abort on
// `ElementAnchor::assertScopedEntityHandleAccessAllowed` (`SVGElement.cc:253`). Tracked by the
// determinism-framework task (#601).
TEST(GlRnrReplayTest, DISABLED_FilteredElementOThenRDragDoesNotPopOBackOnRClick) {
  constexpr std::string_view kRnrPath =
      "donner/editor/tests/filtered-element-flash-after-drags-2.rnr";
  const std::filesystem::path rnrPath = RunfilePath(kRnrPath);
  std::optional<repro::ReproFile> reproFile = repro::ReadReproFile(rnrPath);
  ASSERT_TRUE(reproFile.has_value());
  ASSERT_TRUE(reproFile->metadata.expect.has_value());
  ASSERT_TRUE(reproFile->metadata.expect->cropRect.has_value());
  const repro::ReproExpectation& expect = *reproFile->metadata.expect;
  ASSERT_EQ(expect.cropMode, "document-canvas");
  const std::uint64_t beforeClickFrame = static_cast<std::uint64_t>(expect.minFrameIndex - 1);
  const std::uint64_t firstClickFrame = static_cast<std::uint64_t>(expect.minFrameIndex);
  const std::uint64_t settledClickFrame = static_cast<std::uint64_t>(expect.minFrameIndex + 2);

  repro::GlRnrReplayOptions options;
  options.rnrPath = rnrPath;
  options.svgPathOverride = RunfilePath("donner_splash.svg");
  options.outputDir = DiagnosticOutputDir() / "gl_o_then_r_popback_repro";
  options.captureFrames = {beforeClickFrame, firstClickFrame, settledClickFrame};
  options.maxFrame = static_cast<std::uint64_t>(expect.maxFrameIndex);
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = true;
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;

  std::optional<svg::RendererBitmap> beforeRClick = LoadCaptureBitmap(result, beforeClickFrame);
  std::optional<svg::RendererBitmap> firstRClickFrame = LoadCaptureBitmap(result, firstClickFrame);
  std::optional<svg::RendererBitmap> settledRClickFrame =
      LoadCaptureBitmap(result, settledClickFrame);
  ASSERT_TRUE(beforeRClick.has_value());
  ASSERT_TRUE(firstRClickFrame.has_value());
  ASSERT_TRUE(settledRClickFrame.has_value());

  const PixelCrop oCrop{
      .x = expect.cropRect->x,
      .y = expect.cropRect->y,
      .width = expect.cropRect->width,
      .height = expect.cropRect->height,
  };
  const svg::RendererBitmap firstRClickO = CropBitmap(*firstRClickFrame, oCrop);
  const svg::RendererBitmap settledRClickO = CropBitmap(*settledRClickFrame, oCrop);

  tests::CompareBitmapToBitmap(firstRClickO, settledRClickO,
                               "gl_o_then_r_frame_153_o_crop_vs_frame_155",
                               tests::PixelmatchIdentityParams());

  const std::optional<double> beforeCentroidY = YellowCentroidY(CropBitmap(*beforeRClick, oCrop));
  const std::optional<double> firstCentroidY = YellowCentroidY(firstRClickO);
  const std::optional<double> settledCentroidY = YellowCentroidY(settledRClickO);
  ASSERT_TRUE(beforeCentroidY.has_value());
  ASSERT_TRUE(firstCentroidY.has_value());
  ASSERT_TRUE(settledCentroidY.has_value());
  EXPECT_NEAR(*beforeCentroidY, *settledCentroidY, 1.0)
      << "The two stable frames should agree on the post-O-drag position.";
  EXPECT_NEAR(*firstCentroidY, *settledCentroidY, 1.0)
      << "The first R-click frame should keep O at its post-drag position instead of popping "
         "back for one presented frame.";
}

}  // namespace
}  // namespace donner::editor
