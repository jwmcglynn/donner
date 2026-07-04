#include "donner/editor/repro/GlRnrReplay.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>

#include "donner/base/Box.h"
#include "donner/base/Vector2.h"
#include "donner/editor/EditorShell.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/gui/EditorWindow.h"
#include "donner/editor/repro/ReproFile.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/RendererInterface.h"

#ifdef DONNER_EDITOR_WGPU
#include "donner/svg/renderer/RendererGeode.h"
#endif

namespace donner::editor::repro {
namespace {

using gui::EditorWindow;
using gui::EditorWindowInputOverride;
using gui::EditorWindowOptions;

struct PixelRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct TexturePixelStats {
  int greenPixels = 0;
  int nonTransparentPixels = 0;
};

enum class PixelSnapMode {
  Covering,
  Contained,
};

[[nodiscard]] bool SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

[[nodiscard]] std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return std::nullopt;
  }

  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

[[nodiscard]] std::filesystem::path ResolveSvgPath(const std::filesystem::path& rnrPath,
                                                   const std::string& recordedPath) {
  const std::filesystem::path direct(recordedPath);
  if (std::filesystem::exists(direct)) {
    return direct;
  }

  const std::filesystem::path besideRnr = rnrPath.parent_path() / recordedPath;
  if (std::filesystem::exists(besideRnr)) {
    return besideRnr;
  }

  return direct;
}

struct ReproSvgInput {
  std::filesystem::path displayPath;
  std::string source;
};

[[nodiscard]] std::filesystem::path EmbeddedSvgDisplayPath(const ReproMetadata& metadata) {
  if (!metadata.svgBasename.empty()) {
    return std::filesystem::path(metadata.svgBasename);
  }
  if (!metadata.svgPath.empty()) {
    return std::filesystem::path(metadata.svgPath).filename();
  }
  return "embedded.svg";
}

[[nodiscard]] std::optional<ReproSvgInput> LoadReproSvgInput(const GlRnrReplayOptions& options,
                                                             const ReproMetadata& metadata,
                                                             std::string* error) {
  if (options.svgPathOverride.has_value()) {
    const std::optional<std::string> source = ReadTextFile(*options.svgPathOverride);
    if (!source.has_value()) {
      (void)SetError(error, "could not read SVG " + options.svgPathOverride->string());
      return std::nullopt;
    }
    return ReproSvgInput{
        .displayPath = *options.svgPathOverride,
        .source = *source,
    };
  }

  if (metadata.svgSource.has_value()) {
    return ReproSvgInput{
        .displayPath = EmbeddedSvgDisplayPath(metadata),
        .source = *metadata.svgSource,
    };
  }

  const std::filesystem::path svgPath = ResolveSvgPath(options.rnrPath, metadata.svgPath);
  const std::optional<std::string> source = ReadTextFile(svgPath);
  if (!source.has_value()) {
    (void)SetError(error, "could not read SVG " + svgPath.string());
    return std::nullopt;
  }
  return ReproSvgInput{
      .displayPath = svgPath,
      .source = *source,
  };
}

[[nodiscard]] bool HasLeftMouseDown(const ReproFrame& frame) {
  for (const ReproEvent& event : frame.events) {
    if (event.kind == ReproEvent::Kind::MouseDown && event.mouseButton == 0) {
      return true;
    }
  }

  return false;
}

[[nodiscard]] EditorWindowInputOverride InputFromFrame(const ReproFrame& frame) {
  EditorWindowInputOverride input;
  input.deltaSeconds = frame.deltaMs > 0.0 ? frame.deltaMs / 1000.0 : 1.0 / 60.0;
  input.mousePosition = Vector2d(frame.mouseX, frame.mouseY);
  for (int i = 0; i < static_cast<int>(input.mouseDown.size()); ++i) {
    input.mouseDown[i] = (frame.mouseButtonMask & (1 << i)) != 0;
  }
  input.keyCtrl = (frame.modifiers & (1 << 0)) != 0;
  input.keyShift = (frame.modifiers & (1 << 1)) != 0;
  input.keyAlt = (frame.modifiers & (1 << 2)) != 0;
  input.keySuper = (frame.modifiers & (1 << 3)) != 0;

  for (const ReproEvent& event : frame.events) {
    switch (event.kind) {
      case ReproEvent::Kind::KeyDown: input.keyDownEvents.push_back(event.key); break;
      case ReproEvent::Kind::KeyUp: input.keyUpEvents.push_back(event.key); break;
      case ReproEvent::Kind::Char: input.inputCharacters.push_back(event.codepoint); break;
      case ReproEvent::Kind::Wheel:
        input.mouseWheelH += event.wheelDeltaX;
        input.mouseWheel += event.wheelDeltaY;
        break;
      case ReproEvent::Kind::MouseDown:
      case ReproEvent::Kind::MouseUp:
      case ReproEvent::Kind::Resize:
      case ReproEvent::Kind::Focus: break;
    }
  }

  return input;
}

void QueueRecordedScrollEvents(EditorShell& shell, const ReproFrame& frame) {
  const bool zoomModifierHeld =
      (frame.modifiers & (1 << 0)) != 0 || (frame.modifiers & (1 << 3)) != 0;
  for (const ReproEvent& event : frame.events) {
    if (event.kind != ReproEvent::Kind::Wheel) {
      continue;
    }

    shell.queueScrollEventForReplayForTesting(RenderPaneScrollEvent{
        .scrollDelta = Vector2d(event.wheelDeltaX, event.wheelDeltaY),
        .cursorScreen = Vector2d(frame.mouseX, frame.mouseY),
        .zoomModifierHeld = zoomModifierHeld,
    });
  }
}

[[nodiscard]] bool HasLeftMouseEvent(const ReproFrame& frame, ReproEvent::Kind kind) {
  return std::ranges::any_of(frame.events, [&](const ReproEvent& event) {
    return event.kind == kind && event.mouseButton == 0;
  });
}

[[nodiscard]] std::optional<std::string> LeftMouseDownHitElementId(const ReproFrame& frame) {
  for (const ReproEvent& event : frame.events) {
    if (event.kind == ReproEvent::Kind::MouseDown && event.mouseButton == 0 &&
        event.hit.has_value() && !event.hit->id.empty()) {
      return event.hit->id;
    }
  }

  return std::nullopt;
}

[[nodiscard]] MouseModifiers MouseModifiersFromFrame(const ReproFrame& frame) {
  MouseModifiers modifiers;
  modifiers.shift = (frame.modifiers & (1 << 1)) != 0;
  modifiers.option = (frame.modifiers & (1 << 2)) != 0;
  modifiers.command = (frame.modifiers & ((1 << 0) | (1 << 3))) != 0;
  modifiers.pixelsPerDocUnit =
      frame.viewport.has_value() && frame.viewport->zoom > 0.0 ? frame.viewport->zoom : 1.0;
  return modifiers;
}

[[nodiscard]] std::optional<Vector2d> DocumentPointFromFrame(const ReproFrame& frame) {
  if (frame.mouseDocX.has_value() && frame.mouseDocY.has_value()) {
    return Vector2d(*frame.mouseDocX, *frame.mouseDocY);
  }

  if (!frame.viewport.has_value() || frame.viewport->zoom <= 0.0) {
    return std::nullopt;
  }

  const ReproViewport& viewport = *frame.viewport;
  return Vector2d(viewport.panDocX + (frame.mouseX - viewport.panScreenX) / viewport.zoom,
                  viewport.panDocY + (frame.mouseY - viewport.panScreenY) / viewport.zoom);
}

[[nodiscard]] std::optional<EditorShellDocumentReplayInput> DocumentReplayInputFromFrame(
    const ReproFrame& frame) {
  const std::optional<Vector2d> documentPoint = DocumentPointFromFrame(frame);
  if (!documentPoint.has_value()) {
    return std::nullopt;
  }

  return EditorShellDocumentReplayInput{
      .documentPoint = *documentPoint,
      .leftMouseDown = (frame.mouseButtonMask & 1) != 0,
      .leftMousePressed = HasLeftMouseEvent(frame, ReproEvent::Kind::MouseDown),
      .leftMouseReleased = HasLeftMouseEvent(frame, ReproEvent::Kind::MouseUp),
      .modifiers = MouseModifiersFromFrame(frame),
      .hitElementId = LeftMouseDownHitElementId(frame),
  };
}

[[nodiscard]] EditorWindowInputOverride NonCanvasInputFromFrame(const ReproFrame& frame) {
  EditorWindowInputOverride input = InputFromFrame(frame);
  input.mousePosition = Vector2d(-10000.0, -10000.0);
  input.mouseDown.fill(false);
  return input;
}

[[nodiscard]] ViewportState ViewportFromReproViewport(const ReproViewport& recordedViewport) {
  ViewportState viewport;
  viewport.paneOrigin = Vector2d(recordedViewport.paneOriginX, recordedViewport.paneOriginY);
  viewport.paneSize = Vector2d(recordedViewport.paneSizeW, recordedViewport.paneSizeH);
  viewport.devicePixelRatio = recordedViewport.devicePixelRatio;
  viewport.zoom = recordedViewport.zoom;
  viewport.panDocPoint = Vector2d(recordedViewport.panDocX, recordedViewport.panDocY);
  viewport.panScreenPoint = Vector2d(recordedViewport.panScreenX, recordedViewport.panScreenY);
  viewport.documentViewBox = Box2d::FromXYWH(recordedViewport.viewBoxX, recordedViewport.viewBoxY,
                                             recordedViewport.viewBoxW, recordedViewport.viewBoxH);
  return viewport;
}

[[nodiscard]] std::filesystem::path CapturePath(const GlRnrReplayOptions& options,
                                                const ReproFrame& frame, std::string_view reason) {
  std::string filename = "gl_replay_frame_" + std::to_string(frame.index);
  if (!reason.empty()) {
    filename += "_";
    filename += reason;
  }
  if (const std::string_view suffix = GlRnrReplayCropModeSuffix(options.cropMode);
      !suffix.empty()) {
    filename += "_";
    filename += suffix;
  }
  filename += ".png";
  return options.outputDir / filename;
}

[[nodiscard]] PixelRect LogicalRectToPixelRect(const Box2d& logicalRect,
                                               const svg::RendererBitmap& bitmap,
                                               double devicePixelRatio, PixelSnapMode snapMode) {
  const double left = logicalRect.topLeft.x * devicePixelRatio;
  const double top = logicalRect.topLeft.y * devicePixelRatio;
  const double right = logicalRect.bottomRight.x * devicePixelRatio;
  const double bottom = logicalRect.bottomRight.y * devicePixelRatio;

  const int unclampedX0 = snapMode == PixelSnapMode::Contained ? static_cast<int>(std::ceil(left))
                                                               : static_cast<int>(std::floor(left));
  const int unclampedY0 = snapMode == PixelSnapMode::Contained ? static_cast<int>(std::ceil(top))
                                                               : static_cast<int>(std::floor(top));
  const int unclampedX1 = snapMode == PixelSnapMode::Contained ? static_cast<int>(std::floor(right))
                                                               : static_cast<int>(std::ceil(right));
  const int unclampedY1 = snapMode == PixelSnapMode::Contained
                              ? static_cast<int>(std::floor(bottom))
                              : static_cast<int>(std::ceil(bottom));

  const int x0 = std::clamp(unclampedX0, 0, bitmap.dimensions.x);
  const int y0 = std::clamp(unclampedY0, 0, bitmap.dimensions.y);
  const int x1 = std::clamp(unclampedX1, x0, bitmap.dimensions.x);
  const int y1 = std::clamp(unclampedY1, y0, bitmap.dimensions.y);
  return PixelRect{
      .x = x0,
      .y = y0,
      .width = x1 - x0,
      .height = y1 - y0,
  };
}

[[nodiscard]] std::optional<PixelRect> CropRectForMode(GlRnrReplayCropMode cropMode,
                                                       const ViewportState& viewport,
                                                       const svg::RendererBitmap& bitmap) {
  if (cropMode == GlRnrReplayCropMode::Full) {
    return std::nullopt;
  }

  const Box2d paneRect = Box2d::FromXYWH(viewport.paneOrigin.x, viewport.paneOrigin.y,
                                         viewport.paneSize.x, viewport.paneSize.y);
  Box2d logicalRect = paneRect;
  if (cropMode == GlRnrReplayCropMode::DocumentCanvas) {
    const Box2d imageRect = viewport.imageScreenRect();
    logicalRect.topLeft.x = std::max(paneRect.topLeft.x, imageRect.topLeft.x);
    logicalRect.topLeft.y = std::max(paneRect.topLeft.y, imageRect.topLeft.y);
    logicalRect.bottomRight.x = std::min(paneRect.bottomRight.x, imageRect.bottomRight.x);
    logicalRect.bottomRight.y = std::min(paneRect.bottomRight.y, imageRect.bottomRight.y);
  }

  const PixelSnapMode snapMode = cropMode == GlRnrReplayCropMode::DocumentCanvas
                                     ? PixelSnapMode::Contained
                                     : PixelSnapMode::Covering;
  PixelRect pixelRect =
      LogicalRectToPixelRect(logicalRect, bitmap, viewport.devicePixelRatio, snapMode);
  if (pixelRect.width <= 0 || pixelRect.height <= 0) {
    return std::nullopt;
  }

  return pixelRect;
}

[[nodiscard]] svg::RendererBitmap CropBitmap(const svg::RendererBitmap& bitmap,
                                             const PixelRect& rect) {
  svg::RendererBitmap cropped;
  cropped.dimensions = Vector2i(rect.width, rect.height);
  cropped.rowBytes = static_cast<std::size_t>(rect.width) * 4u;
  cropped.alphaType = bitmap.alphaType;
  cropped.pixels.resize(cropped.rowBytes * static_cast<std::size_t>(rect.height));

  for (int y = 0; y < rect.height; ++y) {
    const uint8_t* src = bitmap.pixels.data() +
                         (static_cast<std::size_t>(rect.y + y) * bitmap.rowBytes) +
                         static_cast<std::size_t>(rect.x) * 4u;
    uint8_t* dst = cropped.pixels.data() + static_cast<std::size_t>(y) * cropped.rowBytes;
    std::memcpy(dst, src, cropped.rowBytes);
  }

  return cropped;
}

[[nodiscard]] svg::RendererBitmap CropForOutput(const svg::RendererBitmap& bitmap,
                                                GlRnrReplayCropMode cropMode,
                                                const ViewportState& viewport) {
  const std::optional<PixelRect> cropRect = CropRectForMode(cropMode, viewport, bitmap);
  return cropRect.has_value() ? CropBitmap(bitmap, *cropRect) : bitmap;
}

#ifdef DONNER_EDITOR_WGPU
std::optional<TexturePixelStats> TexturePixelStatsForSnapshot(
    const std::shared_ptr<const svg::RendererTextureSnapshot>& textureSnapshot,
    const std::shared_ptr<geode::GeodeDevice>& device) {
  if (textureSnapshot == nullptr || device == nullptr ||
      textureSnapshot->backend() != svg::RendererTextureSnapshotBackend::Geode) {
    return std::nullopt;
  }

  const Vector2i dims = textureSnapshot->dimensions();
  if (dims.x <= 0 || dims.y <= 0) {
    return std::nullopt;
  }

  svg::RendererGeode renderer(device);
  svg::RenderViewport viewport;
  viewport.size = Vector2d(static_cast<double>(dims.x), static_cast<double>(dims.y));
  viewport.devicePixelRatio = 1.0;
  renderer.beginFrame(viewport);
  if (!renderer.drawTextureSnapshot(
          *textureSnapshot, Box2d(Vector2d::Zero(), Vector2d(static_cast<double>(dims.x),
                                                             static_cast<double>(dims.y))))) {
    renderer.endFrame();
    return std::nullopt;
  }
  renderer.endFrame();

  const svg::RendererBitmap bitmap = renderer.takeSnapshot();
  if (bitmap.empty()) {
    return std::nullopt;
  }

  TexturePixelStats stats;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const std::size_t offset =
          static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
      const std::uint8_t r = bitmap.pixels[offset + 0u];
      const std::uint8_t g = bitmap.pixels[offset + 1u];
      const std::uint8_t b = bitmap.pixels[offset + 2u];
      const std::uint8_t a = bitmap.pixels[offset + 3u];
      if (a > 16u) {
        ++stats.nonTransparentPixels;
      }
      if (r < 95u && g > 145u && b < 90u && a > 180u) {
        ++stats.greenPixels;
      }
    }
  }
  return stats;
}
#endif

[[nodiscard]] bool WriteCapture(const svg::RendererBitmap& bitmap,
                                const std::filesystem::path& path, std::string* error) {
  if (bitmap.empty()) {
    return SetError(error, "framebuffer readback was empty for " + path.string());
  }

  if (!svg::RendererImageIO::writeRgbaPixelsToPngFile(
          path.string().c_str(), std::span<const uint8_t>(bitmap.pixels), bitmap.dimensions.x,
          bitmap.dimensions.y, bitmap.rowBytes / 4)) {
    return SetError(error, "failed to write " + path.string());
  }

  return true;
}

[[nodiscard]] bool ShouldDrainWorkerBeforeFrame(GlRnrReplayWorkerScheduling scheduling) {
  switch (scheduling) {
    case GlRnrReplayWorkerScheduling::Realtime: return false;
    case GlRnrReplayWorkerScheduling::DrainEachFrame:
    case GlRnrReplayWorkerScheduling::HoldFramesBehind: return true;
  }

  return false;
}

[[nodiscard]] bool WaitForReplayWorkerBeforeFrame(const GlRnrReplayOptions& options,
                                                  EditorShell& shell, const ReproFrame& frame,
                                                  std::string* error) {
  if (!ShouldDrainWorkerBeforeFrame(options.workerScheduling)) {
    return true;
  }

  constexpr std::chrono::seconds kReplayWorkerTimeout(30);
  const auto deadline = std::chrono::steady_clock::now() + kReplayWorkerTimeout;
  if (shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(deadline)) {
    return true;
  }

  return SetError(
      error, "timed out waiting for replay worker before frame " + std::to_string(frame.index));
}

}  // namespace

std::optional<GlRnrReplayCropMode> ParseGlRnrReplayCropMode(std::string_view value) {
  if (value == "full") {
    return GlRnrReplayCropMode::Full;
  }
  if (value == "render-pane") {
    return GlRnrReplayCropMode::RenderPane;
  }
  if (value == "document-canvas" || value == "canvas") {
    return GlRnrReplayCropMode::DocumentCanvas;
  }

  return std::nullopt;
}

std::string_view GlRnrReplayCropModeSuffix(GlRnrReplayCropMode cropMode) {
  switch (cropMode) {
    case GlRnrReplayCropMode::Full: return "";
    case GlRnrReplayCropMode::RenderPane: return "render_pane";
    case GlRnrReplayCropMode::DocumentCanvas: return "canvas";
  }

  return "";
}

bool RunGlRnrReplay(const GlRnrReplayOptions& options, GlRnrReplayResult* result,
                    std::string* error) {
  if (result == nullptr) {
    return SetError(error, "RunGlRnrReplay result pointer must not be null");
  }
  *result = GlRnrReplayResult{};

  if (options.rnrPath.empty()) {
    return SetError(error, "rnrPath is required");
  }
  if (options.captureFrames.empty() && !options.captureLeftMouseDownOrdinal.has_value()) {
    return SetError(error, "at least one GL capture selector is required");
  }
  if (options.holdFramesBehind < 0) {
    return SetError(error, "holdFramesBehind must be non-negative");
  }
  if (options.workerRenderDelayMsForTesting < 0) {
    return SetError(error, "workerRenderDelayMsForTesting must be non-negative");
  }
  if (options.workerScheduling != GlRnrReplayWorkerScheduling::HoldFramesBehind &&
      options.holdFramesBehind != 0) {
    return SetError(error, "holdFramesBehind requires HoldFramesBehind worker scheduling");
  }

  const std::optional<ReproFile> repro = ReadReproFile(options.rnrPath);
  if (!repro.has_value()) {
    return SetError(error, "failed to read .rnr file: " + options.rnrPath.string());
  }

  const std::optional<ReproSvgInput> svgInput = LoadReproSvgInput(options, repro->metadata, error);
  if (!svgInput.has_value()) {
    return false;
  }

  std::error_code createDirError;
  std::filesystem::create_directories(options.outputDir, createDirError);
  if (createDirError) {
    return SetError(error, "could not create output directory " + options.outputDir.string() +
                               ": " + createDirError.message());
  }

  const int initialWidth = repro->metadata.windowWidth > 0 ? repro->metadata.windowWidth : 1600;
  const int initialHeight = repro->metadata.windowHeight > 0 ? repro->metadata.windowHeight : 900;
  const double recordedScale =
      repro->metadata.displayScale > 0.0 ? repro->metadata.displayScale : 1.0;
  EditorWindow window(EditorWindowOptions{
      .title = "Donner RNR GL Replay",
      .initialWidth = initialWidth,
      .initialHeight = initialHeight,
      .visible = options.visible,
      // Capture replays hide their window by default. Linux uses GLFW's null
      // platform with Mesa's EGL/llvmpipe path for framebuffer readback; macOS
      // keeps the native GPU-backed Cocoa context. An interactive (visible)
      // replay keeps the native windowed platform.
      .offscreen = !options.visible,
      // Reproduce the recorded HiDPI scale during hidden replay so framebuffer
      // crops line up with captures taken on the recording host.
      .offscreenContentScale = recordedScale,
      .enableFramebufferReadback = true,
  });
  if (!window.valid()) {
    if (window.glUnavailable()) {
      result->glUnavailable = true;
    }
    return SetError(error, "failed to initialize editor window");
  }

  EditorShell shell(window, EditorShellOptions{
                                .svgPath = svgInput->displayPath.string(),
                                .initialSource = svgInput->source,
                                .initialPath = svgInput->displayPath.string(),
                                .editorNoticeText = "",
                            });
  if (!shell.valid()) {
    return SetError(error, "failed to initialize editor shell");
  }

  AsyncRenderer& replayRenderer = shell.asyncRendererForReplay();
  replayRenderer.setReplayRenderDelayForTesting(
      std::chrono::milliseconds(options.workerRenderDelayMsForTesting));
  if (options.workerScheduling == GlRnrReplayWorkerScheduling::HoldFramesBehind) {
    replayRenderer.setReplayResultHoldFramesForTesting(options.holdFramesBehind);
  }

  int leftMouseDownOrdinal = 0;
  const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

  for (const ReproFrame& frame : repro->frames) {
    if (options.maxFrame.has_value() && frame.index > *options.maxFrame) {
      break;
    }

    if (options.pace) {
      const std::chrono::duration<double> offset(frame.timestampSeconds);
      const std::chrono::steady_clock::time_point target =
          start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(offset);
      std::this_thread::sleep_until(target);
    }

    std::optional<std::string> captureReason;
    if (options.captureFrames.find(frame.index) != options.captureFrames.end()) {
      captureReason = "explicit";
    }
    if (HasLeftMouseDown(frame)) {
      ++leftMouseDownOrdinal;
      if (options.captureLeftMouseDownOrdinal.has_value() &&
          leftMouseDownOrdinal == *options.captureLeftMouseDownOrdinal) {
        captureReason = "left_mousedown_" + std::to_string(leftMouseDownOrdinal);
      }
    }

    if (!WaitForReplayWorkerBeforeFrame(options, shell, frame, error)) {
      return false;
    }

    const std::uint64_t holdPollCountBefore = replayRenderer.replayResultHoldPollCountForTesting();
    window.pollEvents();
    window.beginFrameWithInput(options.driveDocumentSpaceInput ? NonCanvasInputFromFrame(frame)
                                                               : InputFromFrame(frame));
    if (frame.viewport.has_value()) {
      shell.overrideViewportForReplay(ViewportFromReproViewport(*frame.viewport));
    }
    if (options.driveDocumentSpaceInput) {
      const std::optional<EditorShellDocumentReplayInput> documentInput =
          DocumentReplayInputFromFrame(frame);
      if (documentInput.has_value()) {
        shell.queueDocumentSpaceReplayInputForTesting(*documentInput);
      }
    }
    QueueRecordedScrollEvents(shell, frame);
    for (const ReproAction& action : frame.actions) {
      shell.applyReplayActionForTesting(action);
    }
    shell.setContentOnlyCaptureForNextFrameForReplay(options.contentOnlyCapture &&
                                                     captureReason.has_value());
    shell.runFrame();
    const std::uint64_t holdPollCountAfter = replayRenderer.replayResultHoldPollCountForTesting();
    const std::uint64_t holdPollsThisFrame = holdPollCountAfter - holdPollCountBefore;
    const LayerInspectorStatusReadback layerStatus = shell.layerInspectorStatusForReadback();
    GlRnrReplayFrameDiagnostics frameDiagnostics{
        .frameIndex = frame.index,
        .canvasFreshness = layerStatus.canvasFreshness,
        .statusSuffix = layerStatus.statusSuffix,
        .viewportDesiredCanvas = layerStatus.viewportDesiredCanvas,
        .documentCanvas = layerStatus.documentCanvas,
        .compositorCanvas = layerStatus.compositorCanvas,
        .metadataOnlyMissCount = layerStatus.metadataOnlyMissCount,
        .duplicateLiveTextureCount = layerStatus.duplicateLiveTextureCount,
        .documentFrameVersion = layerStatus.documentFrameVersion,
        .displayedDocVersion = layerStatus.displayedDocVersion,
        .immediateOverlayDocumentVersion = layerStatus.immediateOverlayDocumentVersion,
        .selectedCompositedEntity = layerStatus.selectedCompositedEntity,
        .lastFlushAppliedCommands = layerStatus.lastFlushAppliedCommands,
        .lastFlushReplacedDocument = layerStatus.lastFlushReplacedDocument,
        .lastFlushRemovedElements = layerStatus.lastFlushRemovedElements,
        .lastFlushCacheInvalidatedElements = layerStatus.lastFlushCacheInvalidatedElements,
        .requestRenderAtEndOfFrame = layerStatus.requestRenderAtEndOfFrame,
        .pendingSelectedLayerRasterizationEntity =
            layerStatus.pendingSelectedLayerRasterizationEntity,
        .pendingSelectedLayerRasterizationVersion =
            layerStatus.pendingSelectedLayerRasterizationVersion,
        .selectedStyleAttribute = layerStatus.selectedStyleAttribute,
        .selectedLocalStyleFill = layerStatus.selectedLocalStyleFill,
        .selectedComputedFill = layerStatus.selectedComputedFill,
        .selectedRenderingInstanceFill = layerStatus.selectedRenderingInstanceFill,
        .selectedPathDataAttribute = layerStatus.selectedPathDataAttribute,
        .presentationResources = layerStatus.presentationResources,
        .frameCost = layerStatus.frameCost,
        .activeDragPreview = layerStatus.activeDragPreview,
        .displayedDragPreview = layerStatus.displayedDragPreview,
        .replayWorkerScheduling = options.workerScheduling,
        .replayWorkerRenderDelayMsForTesting = options.workerRenderDelayMsForTesting,
        .replayHoldFramesBehind = options.holdFramesBehind,
        .replayResultHoldPollsThisFrame = holdPollsThisFrame,
        .replayResultWithheld = holdPollsThisFrame > 0,
    };
    frameDiagnostics.tiles.reserve(layerStatus.tiles.size());
    for (const LayerInspectorStatusReadback::Tile& tile : layerStatus.tiles) {
      int textureGreenPixels = 0;
      int textureNonTransparentPixels = 0;
      bool hasTextureSnapshot = tile.textureSnapshot != nullptr;
#ifdef DONNER_EDITOR_WGPU
      if (const std::optional<TexturePixelStats> stats =
              TexturePixelStatsForSnapshot(tile.textureSnapshot, window.geodeFramebufferDevice());
          stats.has_value()) {
        textureGreenPixels = stats->greenPixels;
        textureNonTransparentPixels = stats->nonTransparentPixels;
        hasTextureSnapshot = true;
      }
#endif
      frameDiagnostics.tiles.push_back(GlRnrReplayTileDiagnostics{
          .id = tile.id,
          .kind = tile.kind,
          .generation = tile.generation,
          .bitmapDimsPx = tile.bitmapDimsPx,
          .rasterCanvasSize = tile.rasterCanvasSize,
          .canvasOffsetDoc = tile.canvasOffsetDoc,
          .bitmapDimsDoc = tile.bitmapDimsDoc,
          .dragTranslationDoc = tile.dragTranslationDoc,
          .presentedDragTranslationDoc = tile.presentedDragTranslationDoc,
          .documentFromCachedDocument = tile.documentFromCachedDocument,
          .presentedDocumentFromCachedDocument = tile.presentedDocumentFromCachedDocument,
          .textureHandle = tile.textureHandle,
          .textureGreenPixels = textureGreenPixels,
          .textureNonTransparentPixels = textureNonTransparentPixels,
          .hasTextureSnapshot = hasTextureSnapshot,
          .metadataOnly = tile.metadataOnly,
          .isDragTarget = tile.isDragTarget,
      });
    }
    frameDiagnostics.rowThumbnails.reserve(layerStatus.rowThumbnails.size());
    for (const LayerInspectorStatusReadback::RowThumbnail& thumbnail : layerStatus.rowThumbnails) {
      frameDiagnostics.rowThumbnails.push_back(GlRnrReplayFrameDiagnostics::RowThumbnail{
          .displayName = thumbnail.displayName,
          .stableId = thumbnail.stableId,
          .bitmapDimsPx = thumbnail.bitmapDimsPx,
      });
    }
    frameDiagnostics.thumbnailRefreshStats = GlRnrReplayFrameDiagnostics::ThumbnailRefreshStats{
        .documentFrameVersion = layerStatus.thumbnailRefreshStats.documentFrameVersion,
        .rowCount = layerStatus.thumbnailRefreshStats.rowCount,
        .renderedCount = layerStatus.thumbnailRefreshStats.renderedCount,
        .reusedCount = layerStatus.thumbnailRefreshStats.reusedCount,
        .skippedForCanvasInvalidationCount =
            layerStatus.thumbnailRefreshStats.skippedForCanvasInvalidationCount,
        .renderMs = layerStatus.thumbnailRefreshStats.renderMs,
    };
    result->frameDiagnostics.push_back(std::move(frameDiagnostics));

    if (captureReason.has_value()) {
      const std::filesystem::path path = CapturePath(options, frame, *captureReason);
      const svg::RendererBitmap bitmap = CropForOutput(
          window.endFrameAndReadPixels(), options.cropMode, shell.viewportForReadback());
      if (!WriteCapture(bitmap, path, error)) {
        return false;
      }

      std::error_code absoluteError;
      std::filesystem::path absolutePath = std::filesystem::absolute(path, absoluteError);
      if (absoluteError) {
        absolutePath = path;
      }
      result->captures.push_back(GlRnrReplayCapture{
          .frameIndex = frame.index,
          .reason = *captureReason,
          .path = absolutePath,
      });
    } else {
      window.endFrame();
    }
  }

  result->finalSelectedElementLabel = shell.selectedElementLabelForReadback();
  return true;
}

}  // namespace donner::editor::repro
