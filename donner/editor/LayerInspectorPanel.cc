#include "donner/editor/LayerInspectorPanel.h"

#include <cinttypes>
#include <cstdint>
#include <unordered_set>

#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {

namespace {

constexpr float kThumbnailDisplayHeight = 48.0f;

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
    case Reason::CompositingBreakingAncestor: return "CompositingBreakingAncestor";
    case Reason::LayerLimit: return "LayerLimit";
    case Reason::MemoryLimit: return "MemoryLimit";
    case Reason::DescendantPromoted: return "DescendantPromoted";
  }
  return "?";
}

}  // namespace

LayerInspectorPanel::~LayerInspectorPanel() {
  for (auto& [_, entry] : textures_) {
    if (entry.texture != 0) {
      glDeleteTextures(1, &entry.texture);
    }
  }
}

GLuint LayerInspectorPanel::uploadThumbnail(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile) {
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
}

void LayerInspectorPanel::evictAbsentTiles(
    std::span<const svg::compositor::CompositorController::CompositeTileSnapshot> tiles) {
  std::unordered_set<std::string> live;
  live.reserve(tiles.size());
  for (const auto& tile : tiles) {
    live.insert(tile.id);
  }
  for (auto it = textures_.begin(); it != textures_.end();) {
    if (live.find(it->first) == live.end()) {
      if (it->second.texture != 0) {
        glDeleteTextures(1, &it->second.texture);
      }
      it = textures_.erase(it);
    } else {
      ++it;
    }
  }
}

void LayerInspectorPanel::render(
    std::span<const svg::compositor::CompositorController::CompositeTileSnapshot> tiles,
    const svg::compositor::CompositorController::StateSnapshot& state,
    Entity workerCompositorEntity, double viewportZoom, double viewportDpr,
    const Vector2i& viewportDesiredCanvas, const Vector2i& documentCanvas,
    const svg::compositor::CompositorController::FastPathCounters& fastPath) {
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
  // Three-way state: viewport.desired (what should be) vs
  // document.canvasSize() (what the commit pipeline has pushed) vs
  // compositor.staticSegmentsCanvas_ (what the compositor last
  // rasterized). When these diverge, the bug class is:
  //   - desired != document → commit pipeline stalled.
  //   - document != compositor → compositor hasn't re-rasterized at
  //     the new doc canvas yet (only matters transiently).
  const bool commitStalled = viewportDesiredCanvas != documentCanvas;
  const bool rasterizeBehind = documentCanvas != state.canvasSize;
  ImGui::TextColored(
      commitStalled ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImGui::GetStyle().Colors[ImGuiCol_Text],
      "  viewport: zoom=%.3f  dpr=%.3f  → desired %d×%d", viewportZoom, viewportDpr,
      viewportDesiredCanvas.x, viewportDesiredCanvas.y);
  ImGui::TextColored(commitStalled     ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                     : rasterizeBehind ? ImVec4(1.0f, 0.7f, 0.4f, 1.0f)
                                       : ImGui::GetStyle().Colors[ImGuiCol_Text],
                     "  document canvas: %d×%d%s", documentCanvas.x, documentCanvas.y,
                     commitStalled     ? "  ← commit stalled vs desired"
                     : rasterizeBehind ? "  ← compositor not yet re-rasterized"
                                       : "");
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

  ImGui::TextUnformatted("Composite tiles (paint order)");
  if (tiles.empty()) {
    ImGui::TextDisabled("(no tiles)");
    evictAbsentTiles(tiles);
    return;
  }

  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_Resizable;
  if (!ImGui::BeginTable("##composite_tiles_table", 5, kFlags)) {
    return;
  }
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed,
                          kThumbnailDisplayHeight * 1.4f);
  ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 64.0f);
  ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 1.6f);
  ImGui::TableSetupColumn("Bitmap", ImGuiTableColumnFlags_WidthStretch, 1.0f);
  ImGui::TableSetupColumn("Raster", ImGuiTableColumnFlags_WidthStretch, 1.0f);
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
    const GLuint texture = uploadThumbnail(tile);
    if (texture != 0) {
      const float aspect = tile.thumbnailDims.y > 0 ? static_cast<float>(tile.thumbnailDims.x) /
                                                          static_cast<float>(tile.thumbnailDims.y)
                                                    : 1.0f;
      const ImVec2 displaySize(kThumbnailDisplayHeight * aspect, kThumbnailDisplayHeight);
      ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texture)), displaySize);
    } else {
      ImGui::TextDisabled("(none)");
    }

    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(KindLabel(tile.kind));

    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted(tile.label.c_str());

    ImGui::TableSetColumnIndex(3);
    if (tile.hasValidBitmap) {
      ImGui::Text("%d×%d", tile.bitmapDims.x, tile.bitmapDims.y);
    } else {
      ImGui::TextDisabled("(empty)");
    }

    ImGui::TableSetColumnIndex(4);
    using Kind = svg::compositor::CompositorController::CompositeTileSnapshot::Kind;
    if (tile.kind == Kind::Background || tile.kind == Kind::Foreground) {
      // bg/fg are composed, not rasterized. Show the generation so
      // the operator can see when the cache rebuilds.
      ImGui::Text("gen %" PRIu64, tile.generation);
    } else {
      ImGui::Text("%.1fms (gen %" PRIu64 ")", tile.lastRasterizeMs, tile.generation);
      totalRasterMs += tile.lastRasterizeMs;
    }
  }
  ImGui::EndTable();

  ImGui::Separator();
  ImGui::TextDisabled("Total raster (layers + segments): %.1fms — bg/fg are composed",
                      totalRasterMs);

  evictAbsentTiles(tiles);
}

}  // namespace donner::editor
