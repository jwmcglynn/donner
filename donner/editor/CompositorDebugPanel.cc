#include "donner/editor/CompositorDebugPanel.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "donner/editor/EditorTheme.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/LayerInspectorDiagnostics.h"
#ifdef DONNER_EDITOR_WGPU
#include "donner/editor/RuntimeBitmapUpload.h"
#include "donner/editor/gui/UiTextureRegistration.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#endif

namespace donner::editor {

namespace {

constexpr float kThumbnailDisplayHeight = 48.0f;
constexpr std::size_t kTelemetryHistoryLimit = 4096u;

/// True when a tile's CPU thumbnail can be presented at all: its source bitmap carried pixels and
/// the downsample covers a non-empty extent. A zero-area extent has nothing to draw and no upload
/// path accepts it, so such a tile has no preview rather than a stale one.
/// @param tile Composite tile whose thumbnail is being considered.
bool TileHasPresentableThumbnail(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
  return tile.hasValidBitmap && !tile.thumbnailPixels.empty() && tile.thumbnailDims.x > 0 &&
         tile.thumbnailDims.y > 0;
}

#ifndef DONNER_EDITOR_WGPU
/// True when a tile's thumbnail buffer holds a full tightly packed RGBA image of its own extent.
/// The runtime uploader makes this check itself; the OpenGL path hands the buffer straight to the
/// driver, so it has to make it here rather than read past the source.
/// @param tile Composite tile whose thumbnail storage is being checked. Its extent must already
///   be presentable.
bool ThumbnailStorageCoversExtent(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
  constexpr std::size_t kBytesPerPixel = 4u;
  const std::size_t width = static_cast<std::size_t>(tile.thumbnailDims.x);
  const std::size_t height = static_cast<std::size_t>(tile.thumbnailDims.y);
  if (width > std::numeric_limits<std::size_t>::max() / kBytesPerPixel ||
      width * kBytesPerPixel > std::numeric_limits<std::size_t>::max() / height) {
    return false;
  }
  return tile.thumbnailPixels.size() >= width * height * kBytesPerPixel;
}
#endif

#ifdef DONNER_EDITOR_WGPU
constexpr std::size_t kRetiredSnapshotFrameLimit = 3;

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

CompositorDebugPanel::CompositorDebugPanel(
    std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice)
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

CompositorDebugPanel::~CompositorDebugPanel() {
#ifdef DONNER_EDITOR_WGPU
  for (const auto& [_, entry] : textures_) {
    releaseImGuiTexture(entry.texture);
  }
  for (const RetiredSnapshot& retired : pendingRetiredSnapshots_) {
    releaseImGuiTexture(retired.texture);
  }
  for (const RetiredSnapshotBatch& batch : retiredSnapshotFrames_) {
    for (const RetiredSnapshot& retired : batch) {
      releaseImGuiTexture(retired.texture);
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

void CompositorDebugPanel::recordHeuristicTelemetrySamples(
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

std::string CompositorDebugPanel::heuristicTelemetryHistoryJson() const {
  std::string result;
  for (const HeuristicTelemetryHistoryEntry& entry : telemetryHistory_) {
    result += entry.jsonLine;
  }
  return result;
}

void CompositorDebugPanel::dropThumbnailRegistration(const std::string& id) {
  auto it = textures_.find(id);
  if (it == textures_.end()) {
    return;
  }
#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch retiredSnapshots;
  if (it->second.texture != 0) {
    retiredSnapshots.push_back(RetireSnapshot(it->second.texture,
                                              std::move(it->second.textureSnapshot),
                                              std::move(it->second.uploadedTexture)));
  }
  textures_.erase(it);
  retireSnapshots(std::move(retiredSnapshots));
#else
  if (it->second.texture != 0) {
    glDeleteTextures(1, &it->second.texture);
  }
  textures_.erase(it);
#endif
}

CompositorDebugPanel::ThumbnailTextureHandle CompositorDebugPanel::keepPreviousThumbnail(
    const std::string& id) {
  auto it = textures_.find(id);
  if (it == textures_.end()) {
    return 0;
  }
  // An entry that never published anything carries no state worth keeping around.
  if (it->second.texture == 0) {
    textures_.erase(it);
    return 0;
  }
  return it->second.texture;
}

#ifdef DONNER_EDITOR_WGPU
CompositorDebugPanel::ThumbnailTextureHandle CompositorDebugPanel::publishSnapshotThumbnail(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
  {
    ThumbnailTexture& entry = textures_[tile.id];
    const bool acquiredSnapshot =
        entry.textureSnapshot != tile.textureSnapshot || entry.uploadedTexture != nullptr;
    // Registering allocates a slot and holds a backing, so a tile showing the same snapshot as
    // last frame reuses the registration it already published. Registering again every frame
    // would strand one slot and one backing per visible tile per frame.
    if (!acquiredSnapshot) {
      entry.uploadedGeneration = tile.generation;
      entry.width = tile.bitmapDims.x;
      entry.height = tile.bitmapDims.y;
      return entry.texture;
    }
  }

  const ThumbnailTextureHandle texture = registerSnapshotTexture(tile.textureSnapshot.get());
  if (texture == 0) {
    dropThumbnailRegistration(tile.id);
    return 0;
  }

  ThumbnailTexture& entry = textures_[tile.id];
  RetiredSnapshotBatch retiredSnapshots;
  if (entry.texture != 0) {
    retiredSnapshots.push_back(RetireSnapshot(entry.texture, std::move(entry.textureSnapshot),
                                              std::move(entry.uploadedTexture)));
  }
  entry.texture = texture;
  entry.textureSnapshot = tile.textureSnapshot;
  entry.uploadedTexture.reset();
  entry.uploadedGeneration = tile.generation;
  entry.width = tile.bitmapDims.x;
  entry.height = tile.bitmapDims.y;
  retireSnapshots(std::move(retiredSnapshots));
  return entry.texture;
}

CompositorDebugPanel::ThumbnailTextureHandle CompositorDebugPanel::publishCpuThumbnail(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
  {
    const ThumbnailTexture& entry = textures_[tile.id];
    const bool needsUpload = entry.texture == 0 || entry.uploadedTexture == nullptr ||
                             entry.uploadedGeneration != tile.generation ||
                             entry.width != tile.thumbnailDims.x ||
                             entry.height != tile.thumbnailDims.y;
    if (!needsUpload) {
      return entry.texture;
    }
  }

  std::shared_ptr<svg::RendererGeodeTextureSnapshot> uploadedTexture =
      uploadThumbnailPixelsToRuntime(tile.thumbnailPixels, tile.thumbnailDims);
  const ThumbnailTextureHandle texture =
      uploadedTexture != nullptr ? registerSnapshotTexture(uploadedTexture.get()) : 0;
  if (texture == 0) {
    return keepPreviousThumbnail(tile.id);
  }

  ThumbnailTexture& entry = textures_[tile.id];
  RetiredSnapshotBatch retiredSnapshots;
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
}
#endif

CompositorDebugPanel::ThumbnailTextureHandle CompositorDebugPanel::uploadThumbnail(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
#ifdef DONNER_EDITOR_WGPU
  // A backend texture snapshot is presented directly, so only the CPU thumbnail path depends on
  // the downsample extent.
  const bool hasTextureSnapshot = tile.textureSnapshot != nullptr;
  const bool hasPresentablePayload = hasTextureSnapshot || TileHasPresentableThumbnail(tile);
#else
  const bool hasPresentablePayload = TileHasPresentableThumbnail(tile);
#endif
  if (!hasPresentablePayload) {
    dropThumbnailRegistration(tile.id);
    return 0;
  }

#ifdef DONNER_EDITOR_WGPU
  if (hasTextureSnapshot) {
    return publishSnapshotThumbnail(tile);
  }
  return publishCpuThumbnail(tile);
#else
  ThumbnailTexture& entry = textures_[tile.id];
  const bool needsUpload = entry.texture == 0 || entry.uploadedGeneration != tile.generation ||
                           entry.width != tile.thumbnailDims.x ||
                           entry.height != tile.thumbnailDims.y;
  if (!needsUpload) {
    return entry.texture;
  }

  // glTexImage2D reads the whole extent, so a short buffer would read past the source. A refused
  // upload keeps whatever this tile last published, matching the runtime path.
  if (!ThumbnailStorageCoversExtent(tile)) {
    return keepPreviousThumbnail(tile.id);
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

void CompositorDebugPanel::evictAbsentTiles(
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

void CompositorDebugPanel::advancePresentationFrame() {
#ifdef DONNER_EDITOR_WGPU
  if (!pendingRetiredSnapshots_.empty()) {
    retiredSnapshotFrames_.push_back(std::move(pendingRetiredSnapshots_));
    pendingRetiredSnapshots_.clear();
  } else if (!retiredSnapshotFrames_.empty()) {
    retiredSnapshotFrames_.push_back(RetiredSnapshotBatch{});
  }

  while (retiredSnapshotFrames_.size() > kRetiredSnapshotFrameLimit) {
    for (const RetiredSnapshot& retired : retiredSnapshotFrames_.front()) {
      releaseImGuiTexture(retired.texture);
    }
    retiredSnapshotFrames_.pop_front();
  }
#endif
}

void CompositorDebugPanel::render(
    std::span<const svg::compositor::CompositorController::CompositeTileSnapshot> tiles,
    const svg::compositor::CompositorController::StateSnapshot& state,
    Entity workerCompositorEntity, double viewportZoom, double viewportDpr,
    const Vector2i& viewportDesiredCanvas, const Vector2i& documentCanvas,
    const PresentationCoverageDiagnostics& coverageDiagnostics,
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
  ImGui::TextColored(commitStalled
                         ? ImGui::ColorConvertU32ToFloat4(EditorTheme::Active().destructive)
                         : ImGui::GetStyle().Colors[ImGuiCol_Text],
                     "  viewport: zoom=%.3f  dpr=%.3f  → desired %d×%d", viewportZoom, viewportDpr,
                     viewportDesiredCanvas.x, viewportDesiredCanvas.y);
  ImGui::TextColored(commitStalled
                         ? ImGui::ColorConvertU32ToFloat4(EditorTheme::Active().destructive)
                     : rasterizeBehind ? ImVec4(1.0f, 0.7f, 0.4f, 1.0f)
                                       : ImGui::GetStyle().Colors[ImGuiCol_Text],
                     "  document canvas: %d×%d%s", documentCanvas.x, documentCanvas.y,
                     CanvasFreshnessStatusSuffix(canvasFreshness).data());
  ImGui::Text("  coverage: active=%s %d×%d  overview=%s %d×%d",
              coverageDiagnostics.activeTilesViewportBounded ? "viewport-bounded" : "full",
              coverageDiagnostics.activeOutputSizePx.x, coverageDiagnostics.activeOutputSizePx.y,
              coverageDiagnostics.overviewInfillAvailable ? "infill" : "none",
              coverageDiagnostics.overviewOutputSizePx.x,
              coverageDiagnostics.overviewOutputSizePx.y);
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
      .activeTilesViewportBounded = coverageDiagnostics.activeTilesViewportBounded,
      .overviewInfillAvailable = coverageDiagnostics.overviewInfillAvailable,
      .activeRasterDocumentRect = coverageDiagnostics.activeRasterDocumentRect,
      .overviewRasterDocumentRect = coverageDiagnostics.overviewRasterDocumentRect,
      .activeOutputSizePx = coverageDiagnostics.activeOutputSizePx,
      .overviewOutputSizePx = coverageDiagnostics.overviewOutputSizePx,
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

  // No `ImGuiTableFlags_ScrollY`: the table auto-extends to its row count and
  // the panel window's own scrollbar reaches every row plus the totals footer
  // below it. A scrolling table would size its child to the space left after
  // the diagnostics header above, which in a short panel is nothing - the
  // table collapsed to a sliver and the rows became unreachable at any scroll
  // position, because the window's scroll extent stopped at the sliver.
  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable;
  if (!ImGui::BeginTable("##composite_tiles_table", 6, kFlags)) {
    return;
  }
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
  ImGui::TextDisabled("Total raster inventory (layers + segments): %.1fms - bg/fg are composed",
                      totalRasterMs);

  evictAbsentTiles(tiles);
}

#ifdef DONNER_EDITOR_WGPU
CompositorDebugPanel::ThumbnailTextureHandle CompositorDebugPanel::registerSnapshotTexture(
    const svg::RendererTextureSnapshot* textureSnapshot) {
  if (textureSnapshot == nullptr) {
    return 0;
  }
  UiTextureBacking backing;
  const ThumbnailTextureHandle handle = RegisterUiSnapshotTexture(*textureSnapshot, &backing);
  if (handle != 0) {
    registeredBackings_[handle] = std::move(backing);
  }
  return handle;
}

std::shared_ptr<svg::RendererGeodeTextureSnapshot>
CompositorDebugPanel::uploadThumbnailPixelsToRuntime(const std::vector<uint8_t>& pixels,
                                                     const Vector2i& dimensions) {
  return UploadRuntimeBitmap(geodeDevice_, pixels, dimensions,
                             static_cast<std::size_t>(dimensions.x) * 4u,
                             svg::AlphaType::Unpremultiplied, dimensions);
}

CompositorDebugPanel::RetiredSnapshot CompositorDebugPanel::RetireSnapshot(
    ThumbnailTextureHandle texture, std::shared_ptr<const svg::RendererTextureSnapshot> snapshot,
    std::shared_ptr<svg::RendererGeodeTextureSnapshot> uploadedTexture) {
  return RetiredSnapshot{
      .texture = texture,
      .snapshot = std::move(snapshot),
      .uploadedTexture = std::move(uploadedTexture),
  };
}

void CompositorDebugPanel::releaseImGuiTexture(ThumbnailTextureHandle texture) {
  // Retiring refuses new draw data at once and releases the slot after the frames a recorded draw
  // can still be in flight, which is the window this panel already held its snapshots for.
  // RetireUiTexture reports a double release rather than swallowing it.
  const auto backing = registeredBackings_.find(texture);
  if (backing == registeredBackings_.end() || !RetireUiTexture(texture, &backing->second)) {
    return;
  }
  registeredBackings_.erase(backing);
}

void CompositorDebugPanel::retireSnapshots(RetiredSnapshotBatch snapshots) {
  if (snapshots.empty()) {
    return;
  }

  pendingRetiredSnapshots_.insert(pendingRetiredSnapshots_.end(),
                                  std::make_move_iterator(snapshots.begin()),
                                  std::make_move_iterator(snapshots.end()));
}
#endif

}  // namespace donner::editor
