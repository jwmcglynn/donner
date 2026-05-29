#include "donner/editor/LayerInspectorPanel.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/LayerInspectorDiagnostics.h"
#ifdef DONNER_EDITOR_WGPU
#include "backends/imgui_impl_wgpu.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#endif

namespace donner::editor {

namespace {

constexpr float kThumbnailDisplayHeight = 48.0f;
constexpr std::size_t kTelemetryHistoryLimit = 4096u;
#ifdef DONNER_EDITOR_WGPU
constexpr std::size_t kRetiredSnapshotFrameLimit = 3;
constexpr uint32_t kWgpuBytesPerRowAlignment = 256u;

uint32_t AlignWgpuBytesPerRow(uint32_t value) {
  return (value + kWgpuBytesPerRowAlignment - 1u) & ~(kWgpuBytesPerRowAlignment - 1u);
}

ImTextureID TextureViewToImTextureId(const wgpu::TextureView& textureView) {
  const WGPUTextureView rawTextureView = textureView;
  return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(rawTextureView));
}
#endif

const char* KindLabel(svg::compositor::CompositorController::CompositeTileSnapshot::Kind kind) {
  using Kind = svg::compositor::CompositorController::CompositeTileSnapshot::Kind;
  switch (kind) {
    case Kind::Background: return "bg";
    case Kind::Foreground: return "fg";
    case Kind::Segment: return "segment";
    case Kind::Layer: return "layer";
  }
  return "?";
}

const char* RefusalReasonLabel(svg::compositor::CompositorController::PromoteRefusalReason reason) {
  using Reason = svg::compositor::CompositorController::PromoteRefusalReason;
  switch (reason) {
    case Reason::None: return "none";
    case Reason::InvalidEntity: return "InvalidEntity";
    case Reason::LayerLimit: return "LayerLimit";
    case Reason::MemoryLimit: return "MemoryLimit";
    case Reason::DescendantPromoted: return "DescendantPromoted";
  }
  return "?";
}

const char* TileRenderModeLabel(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
  using Kind = svg::compositor::CompositorController::CompositeTileSnapshot::Kind;
  if ((tile.kind == Kind::Segment || tile.kind == Kind::Layer) && tile.immediate) {
    return "direct";
  }

  if (tile.kind == Kind::Background || tile.kind == Kind::Foreground) {
    return "composed";
  }

  return "cached";
}

const char* ImmediateReasonLabel(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
  if (tile.demotedDynamicImmediate) {
    return "demoted";
  }

  if (tile.staticHeuristicImmediate) {
    return "static";
  }

  if (tile.dynamicHeuristicImmediate) {
    return "measured";
  }

  return "";
}

bool ImmediateTileOverBudget(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
  return tile.immediate && tile.immediateBudgetMs > 0.0 &&
         tile.lastRasterizeMs > tile.immediateBudgetMs;
}

double KiBFromBytes(std::uint64_t bytes) {
  return static_cast<double>(bytes) / 1024.0;
}

std::string DefaultTelemetryPath() {
  std::error_code error;
  std::filesystem::path directory = std::filesystem::temp_directory_path(error);
  if (error) {
    directory = ".";
  }

  directory /= "donner-compositor-heuristics.jsonl";
  return directory.string();
}

std::string TelemetrySampleKey(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile,
    const CompositorHeuristicTelemetryContext& context) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(3);
  os << tile.id << '|' << tile.generation << '|' << (tile.immediate ? 1 : 0) << '|'
     << (tile.dynamicHeuristicImmediate ? 1 : 0) << '|' << (tile.demotedDynamicImmediate ? 1 : 0)
     << '|' << tile.lastRasterizeMs << '|' << context.viewportZoom << '|' << context.viewportDpr
     << '|' << context.viewportDesiredCanvas.x << 'x' << context.viewportDesiredCanvas.y << '|'
     << context.documentCanvas.x << 'x' << context.documentCanvas.y << '|'
     << context.state.canvasSize.x << 'x' << context.state.canvasSize.y;
  return os.str();
}

}  // namespace

#ifdef DONNER_EDITOR_WGPU
struct LayerInspectorPanel::WgpuUploadedTexture {
  donner::geode::ScopedWgpuHandle<wgpu::Texture> texture;
  donner::geode::ScopedWgpuHandle<wgpu::TextureView> view;
};
#endif

LayerInspectorPanel::LayerInspectorPanel(std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice)
#ifdef DONNER_EDITOR_WGPU
    : geodeDevice_(std::move(geodeDevice))
#endif
{
#ifndef DONNER_EDITOR_WGPU
  (void)geodeDevice;
#endif
  const std::string defaultTelemetryPath = DefaultTelemetryPath();
  std::strncpy(telemetryPathBuffer_.data(), defaultTelemetryPath.c_str(),
               telemetryPathBuffer_.size() - 1u);
}

LayerInspectorPanel::~LayerInspectorPanel() {
#ifdef DONNER_EDITOR_WGPU
  for (const auto& [_, entry] : textures_) {
    ReleaseImGuiTexture(entry.texture);
  }
  for (const RetiredSnapshot& retired : pendingRetiredSnapshots_) {
    ReleaseImGuiTexture(retired.texture);
  }
  for (const RetiredSnapshotBatch& batch : retiredSnapshotFrames_) {
    for (const RetiredSnapshot& retired : batch) {
      ReleaseImGuiTexture(retired.texture);
    }
  }
#else
  for (auto& [_, entry] : textures_) {
    if (entry.texture != 0) {
      glDeleteTextures(1, &entry.texture);
    }
  }
#endif
}

void LayerInspectorPanel::recordHeuristicTelemetrySamples(
    std::span<const svg::compositor::CompositorController::CompositeTileSnapshot> tiles,
    const CompositorHeuristicTelemetryContext& context) {
  using Kind = svg::compositor::CompositorController::CompositeTileSnapshot::Kind;
  for (const auto& tile : tiles) {
    if (tile.kind != Kind::Segment && tile.kind != Kind::Layer) {
      continue;
    }

    const std::string key = TelemetrySampleKey(tile, context);
    if (telemetryHistoryKeys_.find(key) != telemetryHistoryKeys_.end()) {
      continue;
    }

    const std::string jsonLine =
        BuildCompositorHeuristicTelemetrySampleJson(tile, context, telemetrySequence_++);
    telemetryHistoryKeys_.insert(key);
    telemetryHistory_.push_back(HeuristicTelemetryHistoryEntry{
        .key = key,
        .jsonLine = jsonLine,
    });
    while (telemetryHistory_.size() > kTelemetryHistoryLimit) {
      telemetryHistoryKeys_.erase(telemetryHistory_.front().key);
      telemetryHistory_.pop_front();
    }
  }
}

std::string LayerInspectorPanel::heuristicTelemetryHistoryJson() const {
  std::string result;
  for (const HeuristicTelemetryHistoryEntry& entry : telemetryHistory_) {
    result += entry.jsonLine;
  }
  return result;
}

LayerInspectorPanel::ThumbnailTextureHandle LayerInspectorPanel::uploadThumbnail(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
#ifdef DONNER_EDITOR_WGPU
  const bool hasTextureSnapshot = tile.textureSnapshot != nullptr;
  const bool hasCpuThumbnail =
      !tile.thumbnailPixels.empty() && tile.thumbnailDims.x > 0 && tile.thumbnailDims.y > 0;

  if (!hasTextureSnapshot && !hasCpuThumbnail) {
    auto it = textures_.find(tile.id);
    if (it != textures_.end()) {
      RetiredSnapshotBatch retiredSnapshots;
      if (it->second.texture != 0) {
        retiredSnapshots.push_back(RetireSnapshot(it->second.texture,
                                                  std::move(it->second.textureSnapshot),
                                                  std::move(it->second.uploadedTexture)));
      }
      textures_.erase(it);
      retireSnapshots(std::move(retiredSnapshots));
    }
    return 0;
  }

  auto& entry = textures_[tile.id];
  RetiredSnapshotBatch retiredSnapshots;

  if (hasTextureSnapshot) {
    const ThumbnailTextureHandle texture = ToImTextureId(tile.textureSnapshot.get());
    if (texture == 0) {
      if (entry.texture != 0) {
        retiredSnapshots.push_back(RetireSnapshot(entry.texture, std::move(entry.textureSnapshot),
                                                  std::move(entry.uploadedTexture)));
      }
      textures_.erase(tile.id);
      retireSnapshots(std::move(retiredSnapshots));
      return 0;
    }

    const bool acquiredSnapshot =
        entry.textureSnapshot != tile.textureSnapshot || entry.uploadedTexture != nullptr;
    if (acquiredSnapshot) {
      if (entry.texture != 0) {
        retiredSnapshots.push_back(RetireSnapshot(entry.texture, std::move(entry.textureSnapshot),
                                                  std::move(entry.uploadedTexture)));
      }
      ImGui_ImplWGPU_AddTexturePremultipliedAlphaRef(texture);
      entry.texture = texture;
      entry.textureSnapshot = tile.textureSnapshot;
      entry.uploadedTexture.reset();
    }

    entry.uploadedGeneration = tile.generation;
    entry.width = tile.bitmapDims.x;
    entry.height = tile.bitmapDims.y;
    retireSnapshots(std::move(retiredSnapshots));
    return entry.texture;
  }

  const bool needsUpload = entry.texture == 0 || entry.uploadedTexture == nullptr ||
                           entry.uploadedGeneration != tile.generation ||
                           entry.width != tile.thumbnailDims.x ||
                           entry.height != tile.thumbnailDims.y;
  if (!needsUpload) {
    return entry.texture;
  }

  std::shared_ptr<WgpuUploadedTexture> uploadedTexture =
      uploadThumbnailPixelsToWgpu(tile.thumbnailPixels, tile.thumbnailDims);
  const ThumbnailTextureHandle texture =
      uploadedTexture != nullptr ? TextureViewToImTextureId(uploadedTexture->view.get()) : 0;
  if (texture == 0) {
    if (entry.texture != 0) {
      retiredSnapshots.push_back(RetireSnapshot(entry.texture, std::move(entry.textureSnapshot),
                                                std::move(entry.uploadedTexture)));
    }
    textures_.erase(tile.id);
    retireSnapshots(std::move(retiredSnapshots));
    return 0;
  }

  if (entry.texture != 0) {
    retiredSnapshots.push_back(RetireSnapshot(entry.texture, std::move(entry.textureSnapshot),
                                              std::move(entry.uploadedTexture)));
  }
  entry.texture = texture;
  entry.textureSnapshot.reset();
  entry.uploadedTexture = std::move(uploadedTexture);
  entry.uploadedGeneration = tile.generation;
  entry.width = tile.thumbnailDims.x;
  entry.height = tile.thumbnailDims.y;
  retireSnapshots(std::move(retiredSnapshots));
  return entry.texture;
#else
  if (!tile.hasValidBitmap || tile.thumbnailPixels.empty() || tile.thumbnailDims.x <= 0 ||
      tile.thumbnailDims.y <= 0) {
    return 0;
  }

  auto& entry = textures_[tile.id];
  const bool needsUpload = entry.texture == 0 || entry.uploadedGeneration != tile.generation ||
                           entry.width != tile.thumbnailDims.x ||
                           entry.height != tile.thumbnailDims.y;
  if (!needsUpload) {
    return entry.texture;
  }

  if (entry.texture == 0) {
    glGenTextures(1, &entry.texture);
    glBindTexture(GL_TEXTURE_2D, entry.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    glBindTexture(GL_TEXTURE_2D, entry.texture);
  }

  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tile.thumbnailDims.x, tile.thumbnailDims.y, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, tile.thumbnailPixels.data());
  entry.uploadedGeneration = tile.generation;
  entry.width = tile.thumbnailDims.x;
  entry.height = tile.thumbnailDims.y;
  return entry.texture;
#endif
}

void LayerInspectorPanel::evictAbsentTiles(
    std::span<const svg::compositor::CompositorController::CompositeTileSnapshot> tiles) {
  std::unordered_set<std::string> live;
  live.reserve(tiles.size());
  for (const auto& tile : tiles) {
    live.insert(tile.id);
  }
#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch retiredSnapshots;
#endif
  for (auto it = textures_.begin(); it != textures_.end();) {
    if (live.find(it->first) == live.end()) {
#ifndef DONNER_EDITOR_WGPU
      if (it->second.texture != 0) {
        glDeleteTextures(1, &it->second.texture);
      }
#else
      if (it->second.texture != 0) {
        retiredSnapshots.push_back(RetireSnapshot(it->second.texture,
                                                  std::move(it->second.textureSnapshot),
                                                  std::move(it->second.uploadedTexture)));
      }
#endif
      it = textures_.erase(it);
    } else {
      ++it;
    }
  }
#ifdef DONNER_EDITOR_WGPU
  retireSnapshots(std::move(retiredSnapshots));
#endif
}

void LayerInspectorPanel::advancePresentationFrame() {
#ifdef DONNER_EDITOR_WGPU
  if (!pendingRetiredSnapshots_.empty()) {
    retiredSnapshotFrames_.push_back(std::move(pendingRetiredSnapshots_));
    pendingRetiredSnapshots_.clear();
  } else if (!retiredSnapshotFrames_.empty()) {
    retiredSnapshotFrames_.push_back(RetiredSnapshotBatch{});
  }

  while (retiredSnapshotFrames_.size() > kRetiredSnapshotFrameLimit) {
    for (const RetiredSnapshot& retired : retiredSnapshotFrames_.front()) {
      ReleaseImGuiTexture(retired.texture);
    }
    retiredSnapshotFrames_.pop_front();
  }
#endif
}

void LayerInspectorPanel::render(
    std::span<const svg::compositor::CompositorController::CompositeTileSnapshot> tiles,
    const svg::compositor::CompositorController::StateSnapshot& state,
    Entity workerCompositorEntity, double viewportZoom, double viewportDpr,
    const Vector2i& viewportDesiredCanvas, const Vector2i& documentCanvas,
    const svg::compositor::CompositorController::FastPathCounters& fastPath,
    const svg::compositor::CompositorController::RenderFrameStats& renderStats) {
  ImGui::Text("Fast path: %" PRIu64 " fast / %" PRIu64 " slow / %" PRIu64 " no-dirty",
              fastPath.fastPathFrames, fastPath.slowPathFramesWithDirty, fastPath.noDirtyFrames);

  // Compositor state diagnostic header. Surfaces the invariants that
  // need to hold for the editor's drag fast path to engage. When the
  // user reports "drag hit slow path" or "segment didn't split", the
  // values here pinpoint which link in the chain (selection → worker
  // promote → activeHints → split path) broke.
  ImGui::Text("State: hints=%u  layers=%u  split=%s  canvas=%d×%d", state.activeHintsCount,
              state.layerCount, state.splitPathActive ? "yes" : "no", state.canvasSize.x,
              state.canvasSize.y);
#ifdef DONNER_EDITOR_WGPU
  ImGui::TextUnformatted("Presentation: WebGPU direct textures (CPU readback/upload disabled)");
#endif
  // Three-way state: viewport.desired (what should be) vs
  // document.canvasSize() (what the commit pipeline has pushed) vs
  // compositor.staticSegmentsCanvas_ (what the compositor last
  // rasterized). When these diverge, the bug class is:
  //   - desired != document → commit pipeline stalled.
  //   - document != compositor → compositor hasn't re-rasterized at
  //     the new doc canvas yet (only matters transiently).
  const CanvasFreshness canvasFreshness =
      ClassifyCanvasFreshness(viewportDesiredCanvas, documentCanvas, state.canvasSize);
  const bool commitStalled = canvasFreshness == CanvasFreshness::CommitStalled;
  const bool rasterizeBehind = canvasFreshness == CanvasFreshness::CompositorBehind;
  ImGui::TextColored(
      commitStalled ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImGui::GetStyle().Colors[ImGuiCol_Text],
      "  viewport: zoom=%.3f  dpr=%.3f  → desired %d×%d", viewportZoom, viewportDpr,
      viewportDesiredCanvas.x, viewportDesiredCanvas.y);
  ImGui::TextColored(commitStalled     ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                     : rasterizeBehind ? ImVec4(1.0f, 0.7f, 0.4f, 1.0f)
                                       : ImGui::GetStyle().Colors[ImGuiCol_Text],
                     "  document canvas: %d×%d%s", documentCanvas.x, documentCanvas.y,
                     CanvasFreshnessStatusSuffix(canvasFreshness).data());
  if (state.splitPathActive || workerCompositorEntity != entt::null) {
    ImGui::Text("  drag entity (split=%u, worker=%u)",
                static_cast<unsigned>(state.splitStaticLayersEntity),
                static_cast<unsigned>(workerCompositorEntity));
  }
  if (state.lastPromoteRefusalReason !=
      svg::compositor::CompositorController::PromoteRefusalReason::None) {
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f), "  last promote refused: %s (entity #%u)",
                       RefusalReasonLabel(state.lastPromoteRefusalReason),
                       static_cast<unsigned>(state.lastPromoteRefusalEntity));
  }
  ImGui::Separator();

  double immediateRasterMs = 0.0;
  double cachedRasterMs = 0.0;
  int immediateRasterTiles = 0;
  int cachedRasterTiles = 0;
  for (const auto& tile : tiles) {
    using Kind = svg::compositor::CompositorController::CompositeTileSnapshot::Kind;
    if (tile.kind != Kind::Segment && tile.kind != Kind::Layer) {
      continue;
    }

    if ((tile.kind == Kind::Segment || tile.kind == Kind::Layer) && tile.immediate) {
      immediateRasterMs += tile.lastRasterizeMs;
      ++immediateRasterTiles;
    } else {
      cachedRasterMs += tile.lastRasterizeMs;
      ++cachedRasterTiles;
    }
  }

  ImGui::Text("Raster last frame: rnd-imm %.1fms (%d)  rnd-cache %.1fms (%d)",
              renderStats.immediateRasterizeMs, renderStats.immediateTileCount,
              renderStats.cachedRasterizeMs, renderStats.cachedTileCount);
  ImGui::Text("Raster inventory: immediate %.1fms (%d)  cached %.1fms (%d)", immediateRasterMs,
              immediateRasterTiles, cachedRasterMs, cachedRasterTiles);
  const CompositorHeuristicTelemetryContext telemetryContext{
      .viewportZoom = viewportZoom,
      .viewportDpr = viewportDpr,
      .viewportDesiredCanvas = viewportDesiredCanvas,
      .documentCanvas = documentCanvas,
      .state = state,
      .fastPath = fastPath,
      .renderStats = renderStats,
  };
  recordHeuristicTelemetrySamples(tiles, telemetryContext);
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputText("Telemetry path", telemetryPathBuffer_.data(), telemetryPathBuffer_.size());
  ImGui::TextDisabled("Telemetry history: %zu segment samples", telemetryHistory_.size());
  if (ImGui::Button("Save heuristic telemetry history")) {
    const std::string json = heuristicTelemetryHistoryJson();
    std::string error;
    if (SaveCompositorHeuristicTelemetry(std::string_view(telemetryPathBuffer_.data()), json,
                                         &error)) {
      telemetryStatus_ = "saved " + std::to_string(telemetryHistory_.size()) + " samples";
    } else {
      telemetryStatus_ = std::move(error);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear telemetry")) {
    telemetryHistory_.clear();
    telemetryHistoryKeys_.clear();
    telemetryStatus_ = "cleared telemetry";
  }
  if (!telemetryStatus_.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", telemetryStatus_.c_str());
  }
  ImGui::TextUnformatted("Composite tiles (paint order)");
  if (tiles.empty()) {
    ImGui::TextDisabled("(no tiles)");
    evictAbsentTiles(tiles);
    return;
  }

  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_Resizable;
  if (!ImGui::BeginTable("##composite_tiles_table", 6, kFlags)) {
    return;
  }
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed,
                          kThumbnailDisplayHeight * 1.4f);
  ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 64.0f);
  ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 86.0f);
  ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 1.5f);
  ImGui::TableSetupColumn("Payload", ImGuiTableColumnFlags_WidthStretch, 1.0f);
  ImGui::TableSetupColumn("Raster", ImGuiTableColumnFlags_WidthStretch, 1.1f);
  ImGui::TableHeadersRow();

  double totalRasterMs = 0.0;
  for (const auto& tile : tiles) {
    ImGui::TableNextRow();

    // Highlight the active drag-target layer with a tinted background
    // row so the operator can pick it out at a glance.
    if (tile.isDragTarget) {
      const ImU32 highlight = ImGui::GetColorU32(ImVec4(0.0f, 0.6f, 1.0f, 0.18f));
      ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, highlight);
    }

    ImGui::TableSetColumnIndex(0);
#ifdef DONNER_EDITOR_WGPU
    const ThumbnailTextureHandle texture = uploadThumbnail(tile);
    if (texture != 0) {
      const float aspect = tile.bitmapDims.y > 0 ? static_cast<float>(tile.bitmapDims.x) /
                                                       static_cast<float>(tile.bitmapDims.y)
                                                 : 1.0f;
      const ImVec2 displaySize(kThumbnailDisplayHeight * aspect, kThumbnailDisplayHeight);
      ImGui::Image(texture, displaySize);
    } else {
      ImGui::TextDisabled("(none)");
    }
#else
    const ThumbnailTextureHandle texture = uploadThumbnail(tile);
    if (texture != 0) {
      const float aspect = tile.thumbnailDims.y > 0 ? static_cast<float>(tile.thumbnailDims.x) /
                                                          static_cast<float>(tile.thumbnailDims.y)
                                                    : 1.0f;
      const ImVec2 displaySize(kThumbnailDisplayHeight * aspect, kThumbnailDisplayHeight);
      ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texture)), displaySize);
    } else {
      ImGui::TextDisabled("(none)");
    }
#endif

    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(KindLabel(tile.kind));

    ImGui::TableSetColumnIndex(2);
    const bool overBudget = ImmediateTileOverBudget(tile);
    const bool demoted = tile.demotedDynamicImmediate;
    const ImVec4 modeColor = tile.immediate ? ImVec4(1.0f, 0.65f, 0.25f, 1.0f)
                                            : (demoted ? ImVec4(1.0f, 0.72f, 0.35f, 1.0f)
                                                       : ImGui::GetStyle().Colors[ImGuiCol_Text]);
    ImGui::TextColored(modeColor, "%s", TileRenderModeLabel(tile));
    if (const char* reason = ImmediateReasonLabel(tile); reason[0] != '\0') {
      ImGui::TextDisabled("%s", reason);
    }

    ImGui::TableSetColumnIndex(3);
    ImGui::TextUnformatted(tile.label.c_str());
    if (!tile.spanRangeLabel.empty()) {
      ImGui::TextDisabled("%s", tile.spanRangeLabel.c_str());
    }

    ImGui::TableSetColumnIndex(4);
    if (tile.hasValidBitmap) {
      if (tile.immediate) {
        ImGui::Text("transient %d×%d", tile.bitmapDims.x, tile.bitmapDims.y);
      } else {
        ImGui::Text("retained %d×%d", tile.bitmapDims.x, tile.bitmapDims.y);
      }
      if (tile.estimatedRetainedBytes > 0u) {
        ImGui::TextDisabled("%.0f KiB est", KiBFromBytes(tile.estimatedRetainedBytes));
      }
    } else {
      ImGui::TextDisabled("(empty)");
    }

    ImGui::TableSetColumnIndex(5);
    using Kind = svg::compositor::CompositorController::CompositeTileSnapshot::Kind;
    if (tile.kind == Kind::Background || tile.kind == Kind::Foreground) {
      // bg/fg are composed, not rasterized. Show the generation so
      // the operator can see when the cache rebuilds.
      ImGui::Text("gen %" PRIu64, tile.generation);
    } else {
      if (overBudget || demoted) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%.1fms", tile.lastRasterizeMs);
      } else {
        ImGui::Text("%.1fms", tile.lastRasterizeMs);
      }
      if (tile.immediateBudgetMs > 0.0) {
        ImGui::TextDisabled("budget %.1fms", tile.immediateBudgetMs);
      }
      if (tile.estimatedDrawOps > 0 || tile.estimatedPathVerbs > 0) {
        ImGui::TextDisabled("ops %d / verbs %d", tile.estimatedDrawOps, tile.estimatedPathVerbs);
      }
      ImGui::TextDisabled("gen %" PRIu64, tile.generation);
      totalRasterMs += tile.lastRasterizeMs;
    }
  }
  ImGui::EndTable();

  ImGui::Separator();
  ImGui::TextDisabled("Total raster inventory (layers + segments): %.1fms — bg/fg are composed",
                      totalRasterMs);

  evictAbsentTiles(tiles);
}

#ifdef DONNER_EDITOR_WGPU
LayerInspectorPanel::ThumbnailTextureHandle LayerInspectorPanel::ToImTextureId(
    const svg::RendererTextureSnapshot* textureSnapshot) {
  if (textureSnapshot == nullptr ||
      textureSnapshot->backend() != svg::RendererTextureSnapshotBackend::Geode) {
    return 0;
  }

  const auto* geodeTexture = static_cast<const svg::RendererGeodeTextureSnapshot*>(textureSnapshot);
  const WGPUTextureView textureView = geodeTexture->textureView();
  return static_cast<ThumbnailTextureHandle>(reinterpret_cast<std::uintptr_t>(textureView));
}

std::shared_ptr<LayerInspectorPanel::WgpuUploadedTexture>
LayerInspectorPanel::uploadThumbnailPixelsToWgpu(const std::vector<uint8_t>& pixels,
                                                 const Vector2i& dimensions) {
  if (geodeDevice_ == nullptr || pixels.empty() || dimensions.x <= 0 || dimensions.y <= 0) {
    return nullptr;
  }

  const uint32_t width = static_cast<uint32_t>(dimensions.x);
  const uint32_t height = static_cast<uint32_t>(dimensions.y);
  const uint32_t tightBytesPerRow = width * 4u;
  const uint32_t paddedBytesPerRow = AlignWgpuBytesPerRow(tightBytesPerRow);
  if (pixels.size() < static_cast<std::size_t>(tightBytesPerRow) * height) {
    return nullptr;
  }

  wgpu::TextureDescriptor textureDesc = {};
  textureDesc.label = donner::geode::wgpuLabel("EditorLayerThumbnail");
  textureDesc.size = {width, height, 1};
  textureDesc.mipLevelCount = 1;
  textureDesc.sampleCount = 1;
  textureDesc.dimension = wgpu::TextureDimension::_2D;
  textureDesc.format = wgpu::TextureFormat::RGBA8Unorm;
  textureDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;

  auto uploaded = std::make_shared<WgpuUploadedTexture>();
  uploaded->texture.reset(geodeDevice_->device().createTexture(textureDesc));
  if (!uploaded->texture) {
    return nullptr;
  }
  geodeDevice_->countTexture();

  std::vector<uint8_t> uploadPixels;
  const uint8_t* uploadData = pixels.data();
  std::size_t uploadSize = pixels.size();
  if (tightBytesPerRow != paddedBytesPerRow) {
    uploadPixels.assign(static_cast<std::size_t>(paddedBytesPerRow) * height, 0u);
    for (uint32_t y = 0; y < height; ++y) {
      const uint8_t* src = pixels.data() + static_cast<std::size_t>(y) * tightBytesPerRow;
      uint8_t* dst = uploadPixels.data() + static_cast<std::size_t>(y) * paddedBytesPerRow;
      std::memcpy(dst, src, tightBytesPerRow);
    }
    uploadData = uploadPixels.data();
    uploadSize = uploadPixels.size();
  }

  wgpu::TexelCopyTextureInfo dst = {};
  dst.texture = uploaded->texture.get();
  dst.mipLevel = 0;
  dst.origin = {0, 0, 0};
  dst.aspect = wgpu::TextureAspect::All;

  wgpu::TexelCopyBufferLayout layout = {};
  layout.offset = 0;
  layout.bytesPerRow = paddedBytesPerRow;
  layout.rowsPerImage = height;

  wgpu::Extent3D writeSize = {width, height, 1};
  geodeDevice_->queue().writeTexture(dst, uploadData, uploadSize, layout, writeSize);

  uploaded->view.reset(uploaded->texture.get().createView());
  if (!uploaded->view) {
    return nullptr;
  }
  return uploaded;
}

LayerInspectorPanel::RetiredSnapshot LayerInspectorPanel::RetireSnapshot(
    ThumbnailTextureHandle texture, std::shared_ptr<const svg::RendererTextureSnapshot> snapshot,
    std::shared_ptr<WgpuUploadedTexture> uploadedTexture) {
  return RetiredSnapshot{
      .texture = texture,
      .snapshot = std::move(snapshot),
      .uploadedTexture = std::move(uploadedTexture),
  };
}

void LayerInspectorPanel::ReleaseImGuiTexture(ThumbnailTextureHandle texture) {
  if (texture == 0) {
    return;
  }

  ImGui_ImplWGPU_RemoveTexturePremultipliedAlphaRef(texture);
  ImGui_ImplWGPU_RemoveTexture(texture);
}

void LayerInspectorPanel::retireSnapshots(RetiredSnapshotBatch snapshots) {
  if (snapshots.empty()) {
    return;
  }

  pendingRetiredSnapshots_.insert(pendingRetiredSnapshots_.end(),
                                  std::make_move_iterator(snapshots.begin()),
                                  std::make_move_iterator(snapshots.end()));
}
#endif

}  // namespace donner::editor
