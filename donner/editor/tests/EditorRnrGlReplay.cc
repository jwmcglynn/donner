/// @file
///
/// Standalone CLI wrapper for the shared OpenGL `.rnr` replay harness.

#include <charconv>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>

#include "donner/editor/repro/GlRnrReplay.h"

namespace {

using donner::Vector2d;
using donner::Vector2i;
using donner::editor::FrameCostBreakdown;
using donner::editor::PresentationResourceStats;
using donner::editor::RenderResult;
using donner::editor::repro::GlRnrReplayCapture;
using donner::editor::repro::GlRnrReplayCropMode;
using donner::editor::repro::GlRnrReplayFrameDiagnostics;
using donner::editor::repro::GlRnrReplayOptions;
using donner::editor::repro::GlRnrReplayResult;
using donner::editor::repro::GlRnrReplayTileDiagnostics;
using donner::editor::repro::ParseGlRnrReplayCropMode;
using donner::editor::repro::RunGlRnrReplay;

[[nodiscard]] bool ParseUInt64(std::string_view text, std::uint64_t* value) {
  if (value == nullptr || text.empty()) {
    return false;
  }

  std::uint64_t parsed = 0;
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const std::from_chars_result result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }

  *value = parsed;
  return true;
}

[[nodiscard]] bool ParseInt(std::string_view text, int* value) {
  std::uint64_t parsed = 0;
  if (!ParseUInt64(text, &parsed) || parsed > static_cast<std::uint64_t>(INT_MAX)) {
    return false;
  }

  *value = static_cast<int>(parsed);
  return true;
}

void PrintUsage(std::string_view argv0) {
  std::cerr << "Usage: " << argv0
            << " --rnr <path> [--out-dir <path>] [--capture-frame <n>]...\n"
               "       [--capture-left-mousedown <ordinal>] [--max-frame <n>]\n"
               "       [--crop full|render-pane|document-canvas]\n"
               "       [--worker-delay-ms <n>]\n"
               "       [--worker-scheduling realtime|drain-each-frame|hold-frames-behind]\n"
               "       [--hold-frames-behind <n>]\n"
               "       [--visible] [--no-pace] [--print-diagnostics]\n";
}

[[nodiscard]] bool ParseArgs(int argc, char** argv, GlRnrReplayOptions* options,
                             bool* printDiagnostics) {
  if (options == nullptr) {
    return false;
  }

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    const auto requireValue = [&](std::string_view name) -> std::optional<std::string_view> {
      if (i + 1 >= argc) {
        std::cerr << name << " requires a value\n";
        return std::nullopt;
      }
      return std::string_view(argv[++i]);
    };

    if (arg == "--rnr") {
      const std::optional<std::string_view> value = requireValue(arg);
      if (!value.has_value()) {
        return false;
      }
      options->rnrPath = std::filesystem::path(*value);
      continue;
    }

    if (arg == "--svg") {
      const std::optional<std::string_view> value = requireValue(arg);
      if (!value.has_value()) {
        return false;
      }
      options->svgPathOverride = std::filesystem::path(*value);
      continue;
    }

    if (arg == "--out-dir") {
      const std::optional<std::string_view> value = requireValue(arg);
      if (!value.has_value()) {
        return false;
      }
      options->outputDir = std::filesystem::path(*value);
      continue;
    }

    if (arg == "--capture-frame") {
      const std::optional<std::string_view> value = requireValue(arg);
      std::uint64_t frame = 0;
      if (!value.has_value() || !ParseUInt64(*value, &frame)) {
        std::cerr << "--capture-frame expects a non-negative integer\n";
        return false;
      }
      options->captureFrames.insert(frame);
      continue;
    }

    if (arg == "--capture-left-mousedown") {
      const std::optional<std::string_view> value = requireValue(arg);
      int ordinal = 0;
      if (!value.has_value() || !ParseInt(*value, &ordinal) || ordinal <= 0) {
        std::cerr << "--capture-left-mousedown expects a positive integer\n";
        return false;
      }
      options->captureLeftMouseDownOrdinal = ordinal;
      continue;
    }

    if (arg == "--max-frame") {
      const std::optional<std::string_view> value = requireValue(arg);
      std::uint64_t frame = 0;
      if (!value.has_value() || !ParseUInt64(*value, &frame)) {
        std::cerr << "--max-frame expects a non-negative integer\n";
        return false;
      }
      options->maxFrame = frame;
      continue;
    }

    if (arg == "--crop") {
      const std::optional<std::string_view> value = requireValue(arg);
      const std::optional<GlRnrReplayCropMode> cropMode =
          value.has_value() ? ParseGlRnrReplayCropMode(*value) : std::nullopt;
      if (!cropMode.has_value()) {
        std::cerr << "--crop expects full, render-pane, or document-canvas\n";
        return false;
      }
      options->cropMode = *cropMode;
      continue;
    }

    if (arg == "--capture-render-pane-only") {
      options->cropMode = GlRnrReplayCropMode::RenderPane;
      continue;
    }

    if (arg == "--capture-canvas-only") {
      options->cropMode = GlRnrReplayCropMode::DocumentCanvas;
      continue;
    }

    if (arg == "--visible") {
      options->visible = true;
      continue;
    }

    if (arg == "--no-pace") {
      options->pace = false;
      continue;
    }

    if (arg == "--worker-delay-ms") {
      const std::optional<std::string_view> value = requireValue(arg);
      int delayMs = 0;
      if (!value.has_value() || !ParseInt(*value, &delayMs)) {
        std::cerr << "--worker-delay-ms expects a non-negative integer\n";
        return false;
      }
      options->workerRenderDelayMsForTesting = delayMs;
      continue;
    }

    if (arg == "--worker-scheduling") {
      const std::optional<std::string_view> value = requireValue(arg);
      if (!value.has_value()) {
        return false;
      }

      if (*value == "realtime") {
        options->workerScheduling = donner::editor::repro::GlRnrReplayWorkerScheduling::Realtime;
      } else if (*value == "drain-each-frame") {
        options->workerScheduling =
            donner::editor::repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
      } else if (*value == "hold-frames-behind") {
        options->workerScheduling =
            donner::editor::repro::GlRnrReplayWorkerScheduling::HoldFramesBehind;
      } else {
        std::cerr << "--worker-scheduling expects realtime, drain-each-frame, or "
                     "hold-frames-behind\n";
        return false;
      }
      continue;
    }

    if (arg == "--hold-frames-behind") {
      const std::optional<std::string_view> value = requireValue(arg);
      int frameCount = 0;
      if (!value.has_value() || !ParseInt(*value, &frameCount)) {
        std::cerr << "--hold-frames-behind expects a non-negative integer\n";
        return false;
      }
      options->holdFramesBehind = frameCount;
      continue;
    }

    if (arg == "--print-diagnostics") {
      if (printDiagnostics == nullptr) {
        return false;
      }
      *printDiagnostics = true;
      continue;
    }

    std::cerr << "Unknown argument: " << arg << "\n";
    return false;
  }

  if (options->rnrPath.empty()) {
    std::cerr << "--rnr is required\n";
    return false;
  }

  if (options->captureFrames.empty() && !options->captureLeftMouseDownOrdinal.has_value()) {
    std::cerr << "At least one capture selector is required\n";
    return false;
  }

  return true;
}

void PrintJsonString(std::string_view value) {
  std::cout << "\"";
  for (const char c : value) {
    switch (c) {
      case '\\': std::cout << "\\\\"; break;
      case '"': std::cout << "\\\""; break;
      case '\n': std::cout << "\\n"; break;
      case '\r': std::cout << "\\r"; break;
      case '\t': std::cout << "\\t"; break;
      default: std::cout << c; break;
    }
  }
  std::cout << "\"";
}

std::string_view TileKindName(RenderResult::CompositedTile::Kind kind) {
  switch (kind) {
    case RenderResult::CompositedTile::Kind::Segment: return "segment";
    case RenderResult::CompositedTile::Kind::Layer: return "layer";
    case RenderResult::CompositedTile::Kind::Immediate: return "immediate";
  }

  return "unknown";
}

void PrintVector2i(const Vector2i& value) {
  std::cout << "[" << value.x << "," << value.y << "]";
}

void PrintVector2d(const Vector2d& value) {
  std::cout << "[" << value.x << "," << value.y << "]";
}

void PrintFrameCost(const FrameCostBreakdown& cost) {
  std::cout << "{\"overlay\":{\"capture_ms\":" << cost.overlay.captureMs
            << ",\"draw_ms\":" << cost.overlay.drawMs
            << ",\"snapshot_ms\":" << cost.overlay.snapshotMs
            << ",\"upload_ms\":" << cost.overlay.uploadMs
            << ",\"payload_bytes\":" << cost.overlay.payloadBytes
            << ",\"selected_elements\":" << cost.overlay.selectedElementCount
            << ",\"source_hover_elements\":" << cost.overlay.sourceHoverElementCount
            << ",\"paths\":" << cost.overlay.pathCount
            << ",\"hover_paths\":" << cost.overlay.hoverPathCount
            << ",\"aabbs\":" << cost.overlay.aabbCount
            << ",\"hover_aabbs\":" << cost.overlay.hoverAabbCount
            << ",\"handles\":" << cost.overlay.handleCount
            << ",\"has_marquee\":" << (cost.overlay.hasMarquee ? "true" : "false")
            << ",\"selection_bounds_only\":"
            << (cost.overlay.selectionBoundsOnly ? "true" : "false")
            << ",\"has_live_drag_preview\":" << (cost.overlay.hasLiveDragPreview ? "true" : "false")
            << ",\"has_represented_drag_preview\":"
            << (cost.overlay.hasRepresentedDragPreview ? "true" : "false")
            << ",\"live_drag_translation_doc\":";
  PrintVector2d(cost.overlay.liveDragTranslationDoc);
  std::cout << ",\"represented_drag_translation_doc\":";
  PrintVector2d(cost.overlay.representedDragTranslationDoc);
  std::cout << ",\"canvas_size\":";
  PrintVector2i(cost.overlay.canvasSize);
  std::cout << "},\"composited_upload\":{\"upload_ms\":" << cost.compositedUpload.uploadMs
            << ",\"payload_bytes\":" << cost.compositedUpload.payloadBytes
            << ",\"payload_pixel_area\":" << cost.compositedUpload.payloadPixelArea
            << ",\"tile_pixel_area\":" << cost.compositedUpload.tilePixelArea
            << ",\"tiles\":" << cost.compositedUpload.tileCount
            << ",\"payload_tiles\":" << cost.compositedUpload.payloadTileCount
            << ",\"bitmap_payload_tiles\":" << cost.compositedUpload.bitmapPayloadTileCount
            << ",\"texture_payload_tiles\":" << cost.compositedUpload.texturePayloadTileCount
            << ",\"metadata_only_tiles\":" << cost.compositedUpload.metadataOnlyTileCount
            << ",\"immediate_tiles\":" << cost.compositedUpload.immediateTileCount
            << "},\"composited_render\":{\"immediate_ms\":" << cost.compositedRender.immediateMs
            << ",\"cached_ms\":" << cost.compositedRender.cachedMs
            << ",\"immediate_tiles\":" << cost.compositedRender.immediateTileCount
            << ",\"cached_tiles\":" << cost.compositedRender.cachedTileCount
            << "},\"source_ropes\":{\"layout_ms\":" << cost.sourceRopes.layoutMs
            << ",\"update_ms\":" << cost.sourceRopes.updateMs
            << ",\"draw_ms\":" << cost.sourceRopes.drawMs
            << ",\"candidates\":" << cost.sourceRopes.candidateCount
            << ",\"laid_out\":" << cost.sourceRopes.laidOutCount
            << ",\"culled\":" << cost.sourceRopes.culledCount
            << ",\"drawn\":" << cost.sourceRopes.drawnCount
            << ",\"static_drawn\":" << cost.sourceRopes.staticDrawnCount
            << ",\"active_states\":" << cost.sourceRopes.activeStateCount
            << "},\"document_canvas_commits\":" << cost.documentCanvasCommitCount
            << ",\"last_committed_canvas_size\":";
  PrintVector2i(cost.lastCommittedCanvasSize);
  std::cout << "}";
}

void PrintPresentationResources(const PresentationResourceStats& resources) {
  std::cout << "{\"overlay_bytes\":" << resources.overlayBytes
            << ",\"active_tile_bytes\":" << resources.activeTileBytes
            << ",\"overview_tile_bytes\":" << resources.overviewTileBytes
            << ",\"pending_retired_bytes\":" << resources.pendingRetiredBytes
            << ",\"aged_retired_bytes\":" << resources.agedRetiredBytes
            << ",\"total_tracked_bytes\":" << resources.totalTrackedBytes
            << ",\"peak_tracked_bytes\":" << resources.peakTrackedBytes
            << ",\"active_tile_textures\":" << resources.activeTileTextures
            << ",\"overview_tile_textures\":" << resources.overviewTileTextures
            << ",\"pending_retired_textures\":" << resources.pendingRetiredTextures
            << ",\"aged_retired_textures\":" << resources.agedRetiredTextures
            << ",\"retired_frame_count\":" << resources.retiredFrameCount
            << ",\"largest_allocation_px\":";
  PrintVector2i(resources.largestAllocationPx);
  std::cout << ",\"wgpu_lifetime_texture_creates\":" << resources.wgpuLifetimeTextureCreates
            << ",\"wgpu_lifetime_buffer_creates\":" << resources.wgpuLifetimeBufferCreates << "}";
}

void PrintJson(const GlRnrReplayResult& result, bool printDiagnostics) {
  std::cout << "{\"captures\":[";
  for (std::size_t i = 0; i < result.captures.size(); ++i) {
    const GlRnrReplayCapture& capture = result.captures[i];
    if (i != 0) {
      std::cout << ",";
    }
    std::cout << "{\"frame\":" << capture.frameIndex << ",\"reason\":";
    PrintJsonString(capture.reason);
    std::cout << ",\"path\":";
    PrintJsonString(capture.path.string());
    std::cout << "}";
  }
  std::cout << "]";
  if (result.finalSelectedElementLabel.has_value()) {
    std::cout << ",\"final_selection\":";
    PrintJsonString(*result.finalSelectedElementLabel);
  }
  if (printDiagnostics) {
    std::cout << ",\"frame_diagnostics\":[";
    for (std::size_t i = 0; i < result.frameDiagnostics.size(); ++i) {
      const GlRnrReplayFrameDiagnostics& frame = result.frameDiagnostics[i];
      if (i != 0) {
        std::cout << ",";
      }
      std::cout << "{\"frame\":" << frame.frameIndex
                << ",\"freshness\":" << static_cast<int>(frame.canvasFreshness)
                << ",\"status_suffix\":";
      PrintJsonString(frame.statusSuffix);
      std::cout << ",\"viewport_desired_canvas\":";
      PrintVector2i(frame.viewportDesiredCanvas);
      std::cout << ",\"document_canvas\":";
      PrintVector2i(frame.documentCanvas);
      std::cout << ",\"compositor_canvas\":";
      PrintVector2i(frame.compositorCanvas);
      std::cout << ",\"metadata_only_miss_count\":" << frame.metadataOnlyMissCount
                << ",\"duplicate_live_texture_count\":" << frame.duplicateLiveTextureCount
                << ",\"presentation_resources\":";
      PrintPresentationResources(frame.presentationResources);
      std::cout << ",\"tiles\":[";
      for (std::size_t tileIndex = 0; tileIndex < frame.tiles.size(); ++tileIndex) {
        const GlRnrReplayTileDiagnostics& tile = frame.tiles[tileIndex];
        if (tileIndex != 0) {
          std::cout << ",";
        }
        std::cout << "{\"id\":";
        PrintJsonString(tile.id);
        std::cout << ",\"kind\":";
        PrintJsonString(TileKindName(tile.kind));
        std::cout << ",\"generation\":" << tile.generation << ",\"bitmap_dims_px\":";
        PrintVector2i(tile.bitmapDimsPx);
        std::cout << ",\"raster_canvas_size\":";
        PrintVector2i(tile.rasterCanvasSize);
        std::cout << ",\"canvas_offset_doc\":";
        PrintVector2d(tile.canvasOffsetDoc);
        std::cout << ",\"bitmap_dims_doc\":";
        PrintVector2d(tile.bitmapDimsDoc);
        std::cout << ",\"drag_translation_doc\":";
        PrintVector2d(tile.dragTranslationDoc);
        std::cout << ",\"presented_drag_translation_doc\":";
        PrintVector2d(tile.presentedDragTranslationDoc);
        std::cout << ",\"texture_handle\":" << tile.textureHandle
                  << ",\"metadata_only\":" << (tile.metadataOnly ? "true" : "false")
                  << ",\"is_drag_target\":" << (tile.isDragTarget ? "true" : "false") << "}";
      }
      std::cout << "],\"frame_cost\":";
      PrintFrameCost(frame.frameCost);
      std::cout << "}";
    }
    std::cout << "]";
  }
  std::cout << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

  GlRnrReplayOptions options;
  bool printDiagnostics = false;
  if (!ParseArgs(argc, argv, &options, &printDiagnostics)) {
    PrintUsage(argc > 0 ? argv[0] : "editor_rnr_gl_replay");
    return 2;
  }

  GlRnrReplayResult result;
  std::string error;
  if (!RunGlRnrReplay(options, &result, &error)) {
    std::cerr << error << "\n";
    return 1;
  }

  PrintJson(result, printDiagnostics);
  return 0;
}
