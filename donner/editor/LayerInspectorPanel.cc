#include "donner/editor/LayerInspectorPanel.h"

#include <cinttypes>
#include <cstdint>
#include <unordered_set>

#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {

namespace {

constexpr float kThumbnailDisplayHeight = 48.0f;

}  // namespace

LayerInspectorPanel::~LayerInspectorPanel() {
  for (auto& [_, entry] : textures_) {
    if (entry.texture != 0) {
      glDeleteTextures(1, &entry.texture);
    }
  }
}

GLuint LayerInspectorPanel::uploadThumbnail(
    const svg::compositor::CompositorController::LayerInspectorRow& row) {
  if (!row.hasValidBitmap || row.thumbnailPixels.empty() || row.thumbnailDims.x <= 0 ||
      row.thumbnailDims.y <= 0) {
    return 0;
  }

  auto& entry = textures_[row.entity];
  const bool needsUpload = entry.texture == 0 || entry.uploadedGeneration != row.generation ||
                           entry.width != row.thumbnailDims.x ||
                           entry.height != row.thumbnailDims.y;
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
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, row.thumbnailDims.x, row.thumbnailDims.y, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, row.thumbnailPixels.data());
  entry.uploadedGeneration = row.generation;
  entry.width = row.thumbnailDims.x;
  entry.height = row.thumbnailDims.y;
  return entry.texture;
}

void LayerInspectorPanel::evictAbsentEntities(
    std::span<const svg::compositor::CompositorController::LayerInspectorRow> rows) {
  std::unordered_set<Entity> live;
  live.reserve(rows.size());
  for (const auto& row : rows) {
    live.insert(row.entity);
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
    std::span<const svg::compositor::CompositorController::LayerInspectorRow> rows,
    std::span<const svg::compositor::CompositorController::SegmentInspectorRow> segmentRows,
    const svg::compositor::CompositorController::FastPathCounters& fastPath) {
  ImGui::Text("Fast path: %" PRIu64 " fast / %" PRIu64 " slow / %" PRIu64 " no-dirty",
              fastPath.fastPathFrames, fastPath.slowPathFramesWithDirty, fastPath.noDirtyFrames);
  ImGui::Separator();

  if (rows.empty()) {
    ImGui::TextDisabled("(no compositor layers)");
    evictAbsentEntities(rows);
    return;
  }

  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_Resizable;
  if (!ImGui::BeginTable("##layer_inspector_table", 7, kFlags)) {
    return;
  }
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed,
                          kThumbnailDisplayHeight * 1.4f);
  ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 28.0f);
  ImGui::TableSetupColumn("Entity", ImGuiTableColumnFlags_WidthFixed, 52.0f);
  ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 1.4f);
  ImGui::TableSetupColumn("Bitmap", ImGuiTableColumnFlags_WidthStretch, 1.0f);
  ImGui::TableSetupColumn("Raster", ImGuiTableColumnFlags_WidthStretch, 1.0f);
  ImGui::TableSetupColumn("Gen", ImGuiTableColumnFlags_WidthStretch, 0.7f);
  ImGui::TableHeadersRow();

  for (const auto& row : rows) {
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    const GLuint texture = uploadThumbnail(row);
    if (texture != 0) {
      const float aspect = row.thumbnailDims.y > 0 ? static_cast<float>(row.thumbnailDims.x) /
                                                         static_cast<float>(row.thumbnailDims.y)
                                                   : 1.0f;
      const ImVec2 displaySize(kThumbnailDisplayHeight * aspect, kThumbnailDisplayHeight);
      ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texture)), displaySize);
    } else {
      ImGui::TextDisabled("(none)");
    }

    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%u", row.layerId);

    ImGui::TableSetColumnIndex(2);
    ImGui::Text("#%u", static_cast<std::uint32_t>(row.entity));

    ImGui::TableSetColumnIndex(3);
    ImGui::TextUnformatted(row.fallbackReasonsText.c_str());

    ImGui::TableSetColumnIndex(4);
    if (row.hasValidBitmap) {
      ImGui::Text("%d×%d", row.bitmapSize.x, row.bitmapSize.y);
    } else {
      ImGui::TextDisabled("(empty)");
    }

    ImGui::TableSetColumnIndex(5);
    ImGui::Text("%ux, %.1fms", row.rasterizeCount, row.lastRasterizeMs);

    ImGui::TableSetColumnIndex(6);
    if (row.dirty) {
      ImGui::Text("%" PRIu64 "*", row.generation);
    } else {
      ImGui::Text("%" PRIu64, row.generation);
    }
  }
  ImGui::EndTable();

  ImGui::Separator();
  ImGui::TextDisabled("* = layer dirty next frame");

  evictAbsentEntities(rows);

  // Per-segment table — visible only when at least one segment slot is
  // populated. Static segments dominate per-frame cost on documents like
  // the splash, so the user needs them visible alongside the layers.
  if (!segmentRows.empty()) {
    ImGui::Separator();
    ImGui::TextUnformatted("Static segments");
    double totalSegmentMs = 0.0;
    for (const auto& s : segmentRows) {
      totalSegmentMs += s.lastRasterizeMs;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(total last-rasterize: %.1fms)", totalSegmentMs);

    constexpr ImGuiTableFlags kSegFlags = ImGuiTableFlags_SizingStretchProp |
                                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                          ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##segment_inspector_table", 5, kSegFlags)) {
      ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.0f);
      ImGui::TableSetupColumn("Bitmap", ImGuiTableColumnFlags_WidthStretch, 1.2f);
      ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn("Raster", ImGuiTableColumnFlags_WidthStretch, 0.9f);
      ImGui::TableSetupColumn("Gen", ImGuiTableColumnFlags_WidthStretch, 0.7f);
      ImGui::TableHeadersRow();
      for (const auto& seg : segmentRows) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%zu", seg.slotIndex);
        ImGui::TableSetColumnIndex(1);
        if (seg.hasValidBitmap) {
          ImGui::Text("%d×%d", seg.bitmapSize.x, seg.bitmapSize.y);
        } else {
          ImGui::TextDisabled("(empty)");
        }
        ImGui::TableSetColumnIndex(2);
        if (seg.canvasOffset.x == 0.0 && seg.canvasOffset.y == 0.0) {
          ImGui::TextDisabled("origin");
        } else {
          ImGui::Text("%.0f,%.0f", seg.canvasOffset.x, seg.canvasOffset.y);
        }
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%.1fms", seg.lastRasterizeMs);
        ImGui::TableSetColumnIndex(4);
        if (seg.dirty) {
          ImGui::Text("%" PRIu64 "*", seg.generation);
        } else {
          ImGui::Text("%" PRIu64, seg.generation);
        }
      }
      ImGui::EndTable();
    }
  }
}

}  // namespace donner::editor
