#include "donner/editor/FrameMissTelemetry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <ostream>
#include <sstream>
#include <string_view>

namespace donner::editor {
namespace {

constexpr double kFrameBudget120Ms = 1000.0 / 120.0;
constexpr double kFrameBudget60Ms = 1000.0 / 60.0;

struct Contributor {
  std::string_view name;
  double ms = 0.0;
};

double SourceRopesMs(const FrameCostBreakdown& cost) {
  return cost.sourceRopes.layoutMs + cost.sourceRopes.updateMs + cost.sourceRopes.drawMs;
}

double KnownProfilerCostMs(const FrameCostBreakdown& cost) {
  return KnownUiFrameCostMs(cost) + KnownAsyncWorkerCostMs(cost);
}

void WriteContributor(std::ostream& out, const Contributor& contributor) {
  out << "{\"name\":\"" << contributor.name << "\",\"ms\":" << contributor.ms << "}";
}

void WriteVector2i(std::ostream& out, const Vector2i& value) {
  out << "[" << value.x << "," << value.y << "]";
}

void WriteVector2d(std::ostream& out, const Vector2d& value) {
  out << "[" << value.x << "," << value.y << "]";
}

template <std::size_t Size>
void WriteContributorArray(std::ostream& out, const std::array<Contributor, Size>& contributors,
                           std::size_t count) {
  out << "[";
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0u) {
      out << ",";
    }
    WriteContributor(out, contributors[i]);
  }
  out << "]";
}

template <std::size_t Size>
void SortContributors(std::array<Contributor, Size>* contributors, std::size_t count) {
  std::sort(contributors->begin(), contributors->begin() + static_cast<std::ptrdiff_t>(count),
            [](const Contributor& lhs, const Contributor& rhs) {
              if (lhs.ms == rhs.ms) {
                return lhs.name < rhs.name;
              }
              return lhs.ms > rhs.ms;
            });
}

template <std::size_t Size>
void AddContributor(std::array<Contributor, Size>* contributors, std::size_t* count,
                    std::string_view name, double ms) {
  if (!(ms > 0.0) || *count >= Size) {
    return;
  }

  (*contributors)[*count] = Contributor{.name = name, .ms = ms};
  ++(*count);
}

void WriteCostDetails(std::ostream& out, const FrameCostBreakdown& cost) {
  out << "\"overlay\":{\"capture_ms\":" << cost.overlay.captureMs
      << ",\"draw_ms\":" << cost.overlay.drawMs << ",\"snapshot_ms\":" << cost.overlay.snapshotMs
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
      << ",\"selection_bounds_only\":" << (cost.overlay.selectionBoundsOnly ? "true" : "false")
      << ",\"has_live_drag_preview\":" << (cost.overlay.hasLiveDragPreview ? "true" : "false")
      << ",\"has_represented_drag_preview\":"
      << (cost.overlay.hasRepresentedDragPreview ? "true" : "false")
      << ",\"live_drag_translation_doc\":";
  WriteVector2d(out, cost.overlay.liveDragTranslationDoc);
  out << ",\"represented_drag_translation_doc\":";
  WriteVector2d(out, cost.overlay.representedDragTranslationDoc);
  out << ",\"canvas_size\":";
  WriteVector2i(out, cost.overlay.canvasSize);
  out << "},";

  out << "\"composited_upload\":{\"upload_ms\":" << cost.compositedUpload.uploadMs
      << ",\"payload_bytes\":" << cost.compositedUpload.payloadBytes
      << ",\"payload_pixel_area\":" << cost.compositedUpload.payloadPixelArea
      << ",\"tile_pixel_area\":" << cost.compositedUpload.tilePixelArea
      << ",\"tiles\":" << cost.compositedUpload.tileCount
      << ",\"payload_tiles\":" << cost.compositedUpload.payloadTileCount
      << ",\"bitmap_payload_tiles\":" << cost.compositedUpload.bitmapPayloadTileCount
      << ",\"texture_payload_tiles\":" << cost.compositedUpload.texturePayloadTileCount
      << ",\"metadata_only_tiles\":" << cost.compositedUpload.metadataOnlyTileCount
      << ",\"immediate_tiles\":" << cost.compositedUpload.immediateTileCount << "},";

  out << "\"composited_render\":{\"immediate_ms\":" << cost.compositedRender.immediateMs
      << ",\"cached_ms\":" << cost.compositedRender.cachedMs
      << ",\"immediate_tiles\":" << cost.compositedRender.immediateTileCount
      << ",\"cached_tiles\":" << cost.compositedRender.cachedTileCount << "},";

  out << "\"source_ropes\":{\"layout_ms\":" << cost.sourceRopes.layoutMs
      << ",\"update_ms\":" << cost.sourceRopes.updateMs
      << ",\"draw_ms\":" << cost.sourceRopes.drawMs
      << ",\"candidates\":" << cost.sourceRopes.candidateCount
      << ",\"laid_out\":" << cost.sourceRopes.laidOutCount
      << ",\"culled\":" << cost.sourceRopes.culledCount
      << ",\"drawn\":" << cost.sourceRopes.drawnCount
      << ",\"static_drawn\":" << cost.sourceRopes.staticDrawnCount
      << ",\"active_states\":" << cost.sourceRopes.activeStateCount << "},";

  out << "\"document_canvas_commits\":" << cost.documentCanvasCommitCount
      << ",\"last_committed_canvas_size\":";
  WriteVector2i(out, cost.lastCommittedCanvasSize);
}

void WriteResourceDetails(std::ostream& out, const FrameMissResourceTelemetry& resources) {
  out << "\"resources\":{\"overlay_bytes\":" << resources.overlayBytes
      << ",\"active_tile_bytes\":" << resources.activeTileBytes
      << ",\"overview_tile_bytes\":" << resources.overviewTileBytes
      << ",\"retired_bytes\":" << resources.retiredBytes
      << ",\"total_tracked_bytes\":" << resources.totalTrackedBytes
      << ",\"peak_tracked_bytes\":" << resources.peakTrackedBytes
      << ",\"wgpu_texture_creates\":" << resources.wgpuLifetimeTextureCreates
      << ",\"wgpu_buffer_creates\":" << resources.wgpuLifetimeBufferCreates << "}";
}

}  // namespace

FrameBudgetMiss ClassifyFrameBudgetMiss(double frameMs) {
  if (!(frameMs > kFrameBudget120Ms)) {
    return FrameBudgetMiss::WithinBudget;
  }

  if (frameMs > kFrameBudget60Ms) {
    return FrameBudgetMiss::Missed60Hz;
  }

  return FrameBudgetMiss::Missed120Hz;
}

const char* FrameBudgetMissName(FrameBudgetMiss miss) {
  switch (miss) {
    case FrameBudgetMiss::WithinBudget: return "within_budget";
    case FrameBudgetMiss::Missed120Hz: return "missed_120hz";
    case FrameBudgetMiss::Missed60Hz: return "missed_60hz";
  }

  return "unknown";
}

double KnownUiFrameCostMs(const FrameCostBreakdown& cost) {
  return cost.overlay.captureMs + cost.overlay.drawMs + cost.overlay.snapshotMs +
         cost.overlay.uploadMs + cost.compositedUpload.uploadMs + SourceRopesMs(cost);
}

double KnownAsyncWorkerCostMs(const FrameCostBreakdown& cost) {
  return cost.compositedRender.immediateMs + cost.compositedRender.cachedMs;
}

std::string BuildFrameMissTelemetryJson(const FrameMissTelemetryInput& input) {
  const FrameBudgetMiss miss = ClassifyFrameBudgetMiss(input.frameMs);
  if (miss == FrameBudgetMiss::WithinBudget) {
    return std::string();
  }

  const double knownUiMs = KnownUiFrameCostMs(input.frameCost);
  const double knownWorkerMs = KnownAsyncWorkerCostMs(input.frameCost);
  const double knownProfilerMs = KnownProfilerCostMs(input.frameCost);
  const double otherUiMs = std::max(0.0, input.frameMs - knownUiMs);

  std::array<Contributor, 12> contributors;
  std::size_t contributorCount = 0;
  AddContributor(&contributors, &contributorCount, "other", otherUiMs);
  AddContributor(&contributors, &contributorCount, "overlay-capture",
                 input.frameCost.overlay.captureMs);
  AddContributor(&contributors, &contributorCount, "overlay-draw", input.frameCost.overlay.drawMs);
  AddContributor(&contributors, &contributorCount, "overlay-snapshot",
                 input.frameCost.overlay.snapshotMs);
  AddContributor(&contributors, &contributorCount, "overlay-upload",
                 input.frameCost.overlay.uploadMs);
  AddContributor(&contributors, &contributorCount, "composited-upload",
                 input.frameCost.compositedUpload.uploadMs);
  AddContributor(&contributors, &contributorCount, "rnd-imm",
                 input.frameCost.compositedRender.immediateMs);
  AddContributor(&contributors, &contributorCount, "rnd-cache",
                 input.frameCost.compositedRender.cachedMs);
  AddContributor(&contributors, &contributorCount, "source-rope-layout",
                 input.frameCost.sourceRopes.layoutMs);
  AddContributor(&contributors, &contributorCount, "source-rope-update",
                 input.frameCost.sourceRopes.updateMs);
  AddContributor(&contributors, &contributorCount, "source-rope-draw",
                 input.frameCost.sourceRopes.drawMs);
  AddContributor(&contributors, &contributorCount, "backend", input.backendMs);
  SortContributors(&contributors, contributorCount);

  std::ostringstream out;
  out << "{\"event\":\"frame_budget_miss\",\"frame\":" << input.frameIndex << ",\"miss\":\""
      << FrameBudgetMissName(miss) << "\",\"frame_ms\":" << input.frameMs
      << ",\"budget_120hz_ms\":" << kFrameBudget120Ms << ",\"budget_60hz_ms\":" << kFrameBudget60Ms
      << ",\"known_ui_ms\":" << knownUiMs << ",\"known_worker_ms\":" << knownWorkerMs
      << ",\"known_profiler_ms\":" << knownProfilerMs << ",\"other_ui_ms\":" << otherUiMs
      << ",\"backend_ms\":" << input.backendMs << ",\"top_contributors\":";
  WriteContributorArray(out, contributors, contributorCount);
  out << ",\"cost\":{";
  WriteCostDetails(out, input.frameCost);
  out << "},";
  WriteResourceDetails(out, input.resources);
  out << "}\n";
  return out.str();
}

}  // namespace donner::editor
