#include "donner/editor/repro/GlRnrReplay.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    if (event.kind == ReproEvent::Kind::Wheel) {
      input.mouseWheelH += event.wheelDeltaX;
      input.mouseWheel += event.wheelDeltaY;
    }
  }

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
                                               double devicePixelRatio) {
  const int x0 = std::clamp(static_cast<int>(std::floor(logicalRect.topLeft.x * devicePixelRatio)),
                            0, bitmap.dimensions.x);
  const int y0 = std::clamp(static_cast<int>(std::floor(logicalRect.topLeft.y * devicePixelRatio)),
                            0, bitmap.dimensions.y);
  const int x1 =
      std::clamp(static_cast<int>(std::ceil(logicalRect.bottomRight.x * devicePixelRatio)), x0,
                 bitmap.dimensions.x);
  const int y1 =
      std::clamp(static_cast<int>(std::ceil(logicalRect.bottomRight.y * devicePixelRatio)), y0,
                 bitmap.dimensions.y);
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

  PixelRect pixelRect = LogicalRectToPixelRect(logicalRect, bitmap, viewport.devicePixelRatio);
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

  const std::optional<ReproFile> repro = ReadReproFile(options.rnrPath);
  if (!repro.has_value()) {
    return SetError(error, "failed to read .rnr file: " + options.rnrPath.string());
  }

  const std::filesystem::path svgPath =
      options.svgPathOverride.value_or(ResolveSvgPath(options.rnrPath, repro->metadata.svgPath));
  const std::optional<std::string> source = ReadTextFile(svgPath);
  if (!source.has_value()) {
    return SetError(error, "could not read SVG " + svgPath.string());
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
      // platform + OSMesa for framebuffer readback; macOS keeps the native
      // GPU-backed Cocoa context. An interactive (visible) replay keeps the
      // native windowed platform.
      .offscreen = !options.visible,
      // Reproduce the recorded HiDPI scale during hidden replay so framebuffer
      // crops line up with captures taken on the recording host.
      .offscreenContentScale = recordedScale,
      .enableFramebufferReadback = true,
  });
  if (!window.valid()) {
    return SetError(error, "failed to initialize editor window");
  }

  EditorShell shell(window, EditorShellOptions{
                                .svgPath = svgPath.string(),
                                .initialSource = source,
                                .initialPath = svgPath.string(),
                                .editorNoticeText = "",
                            });
  if (!shell.valid()) {
    return SetError(error, "failed to initialize editor shell");
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

    window.pollEvents();
    window.beginFrameWithInput(InputFromFrame(frame));
    if (frame.viewport.has_value()) {
      shell.overrideViewportForReplay(ViewportFromReproViewport(*frame.viewport));
    }
    shell.runFrame();
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
        .overlayDimsPx = layerStatus.overlayDimsPx,
        .overlayTextureHandle = layerStatus.overlayTextureHandle,
    };
    frameDiagnostics.tiles.reserve(layerStatus.tiles.size());
    for (const LayerInspectorStatusReadback::Tile& tile : layerStatus.tiles) {
      frameDiagnostics.tiles.push_back(GlRnrReplayTileDiagnostics{
          .id = tile.id,
          .kind = tile.kind,
          .generation = tile.generation,
          .bitmapDimsPx = tile.bitmapDimsPx,
          .rasterCanvasSize = tile.rasterCanvasSize,
          .canvasOffsetDoc = tile.canvasOffsetDoc,
          .bitmapDimsDoc = tile.bitmapDimsDoc,
          .dragTranslationDoc = tile.dragTranslationDoc,
          .textureHandle = tile.textureHandle,
          .metadataOnly = tile.metadataOnly,
          .isDragTarget = tile.isDragTarget,
      });
    }
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
