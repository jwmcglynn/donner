#include "donner/editor/GlTextureCache.h"

#include <iterator>

#include "donner/editor/TracyWrapper.h"
#ifdef DONNER_EDITOR_WGPU
#include "backends/imgui_impl_wgpu.h"
#include "donner/svg/renderer/RendererGeode.h"
#endif

namespace donner::editor {

namespace {

#ifdef DONNER_EDITOR_WGPU
constexpr std::size_t kRetiredSnapshotFrameLimit = 3;
#endif

Vector2i PayloadDimensionsForTile(const RenderResult::CompositedTile& tile) {
  if (!tile.bitmap.empty()) {
    return tile.bitmap.dimensions;
  }
  if (tile.textureSnapshot != nullptr) {
    return tile.textureSnapshot->dimensions();
  }
  return tile.bitmapDimsPx;
}

bool TileHasPayload(const RenderResult::CompositedTile& tile) {
  return !tile.bitmap.empty() || tile.textureSnapshot != nullptr;
}

}  // namespace

CompositedTileTextureIdentity TextureIdentityForCompositedTile(
    const RenderResult::CompositedTile& tile) {
  return CompositedTileTextureIdentity{
      .kind = tile.kind,
      .generation = tile.generation,
      .textureDimsPx = PayloadDimensionsForTile(tile),
      .rasterCanvasSize = tile.rasterCanvasSize,
  };
}

bool TextureIdentityMatchesCompositedTile(const CompositedTileTextureIdentity& cachedIdentity,
                                          const RenderResult::CompositedTile& tile) {
  return cachedIdentity == TextureIdentityForCompositedTile(tile);
}

GlTextureCache::~GlTextureCache() {
#ifdef DONNER_EDITOR_WGPU
  releaseImGuiTexture(overlayTexture_);
  for (const auto& [_, entry] : tileTextures_) {
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
  if (overlayTexture_ != 0) {
    glDeleteTextures(1, &overlayTexture_);
  }
  for (auto& [_, entry] : tileTextures_) {
    if (entry.texture != 0) {
      glDeleteTextures(1, &entry.texture);
    }
  }
#endif
}

void GlTextureCache::initialize() {
#ifndef DONNER_EDITOR_WGPU
  if (overlayTexture_ == 0) {
    glGenTextures(1, &overlayTexture_);
    InitializeTexture(overlayTexture_);
  }
#endif
}

void GlTextureCache::uploadOverlay(const svg::RendererBitmap& bitmap) {
  ZoneScopedN("GlTextureCache::uploadOverlay");
#ifdef DONNER_EDITOR_WGPU
  (void)bitmap;
  clearOverlay();
#else
  UploadBitmap(overlayTexture_, bitmap, &overlayWidth_, &overlayHeight_);
#endif
}

void GlTextureCache::uploadOverlayTexture(
    std::shared_ptr<const svg::RendererTextureSnapshot> textureSnapshot) {
  ZoneScopedN("GlTextureCache::uploadOverlayTexture");
#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch retiredSnapshots;
  if (overlayTextureSnapshot_ != nullptr && overlayTextureSnapshot_ != textureSnapshot) {
    retiredSnapshots.push_back(RetireSnapshot(overlayTexture_, std::move(overlayTextureSnapshot_)));
  }
  overlayTextureSnapshot_ = std::move(textureSnapshot);
  overlayTexture_ = ToImTextureId(overlayTextureSnapshot_.get());
  if (overlayTextureSnapshot_ != nullptr && overlayTexture_ != 0) {
    const Vector2i dims = overlayTextureSnapshot_->dimensions();
    overlayWidth_ = dims.x;
    overlayHeight_ = dims.y;
  } else {
    if (overlayTextureSnapshot_ != nullptr) {
      retiredSnapshots.push_back(
          RetireSnapshot(overlayTexture_, std::move(overlayTextureSnapshot_)));
    }
    overlayTexture_ = 0;
    overlayWidth_ = 0;
    overlayHeight_ = 0;
  }
  retireSnapshots(std::move(retiredSnapshots));
#else
  (void)textureSnapshot;
  clearOverlay();
#endif
}

void GlTextureCache::uploadComposited(const RenderResult::CompositedPreview& preview) {
  ZoneScopedN("GlTextureCache::uploadComposited");

  // Track which ids appear in the new snapshot so we can evict
  // textures whose tile has disappeared (drag-target switch demoted a
  // layer, segment cache shrank, etc.).
  std::unordered_set<std::string> liveIds;
  liveIds.reserve(preview.tiles.size());

  tiles_.clear();
  tiles_.reserve(preview.tiles.size());
  metadataOnlyMissCount_ = 0;
  duplicateLiveTextureCount_ = 0;

#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch retiredSnapshots;
#endif

  for (const auto& tile : preview.tiles) {
    // Partial tile snapshots can carry metadata without a pixel payload.
    // Preserve the already-uploaded texture for that tile id and update only
    // its presentation geometry.
    if (!TileHasPayload(tile)) {
      auto it = tileTextures_.find(tile.id);
      if (it == tileTextures_.end() ||
          !TextureIdentityMatchesCompositedTile(it->second.identity, tile)) {
        ++metadataOnlyMissCount_;
        continue;
      }
      liveIds.insert(tile.id);
      TileView view;
      view.texture = ToImTextureId(it->second.texture);
      view.id = tile.id;
      view.kind = tile.kind;
      view.generation = tile.generation;
      view.bitmapDimsPx = Vector2i(it->second.width, it->second.height);
      view.rasterCanvasSize = tile.rasterCanvasSize;
      view.canvasOffsetDoc = tile.canvasOffsetDoc;
      view.bitmapDimsDoc = tile.bitmapDimsDoc;
      view.dragTranslationDoc = tile.dragTranslationDoc;
      view.metadataOnly = true;
      view.isDragTarget = tile.isDragTarget;
      tiles_.push_back(view);
      continue;
    }

#ifdef DONNER_EDITOR_WGPU
    if (tile.textureSnapshot == nullptr) {
      continue;
    }
    const ImTextureID textureId = ToImTextureId(tile.textureSnapshot.get());
    if (textureId == 0) {
      continue;
    }
    liveIds.insert(tile.id);
    auto& entry = tileTextures_[tile.id];
    const Vector2i textureDims = tile.textureSnapshot->dimensions();
    if (entry.textureSnapshot != nullptr && entry.textureSnapshot != tile.textureSnapshot) {
      retiredSnapshots.push_back(RetireSnapshot(entry.texture, std::move(entry.textureSnapshot)));
    }
    entry.texture = textureId;
    entry.textureSnapshot = tile.textureSnapshot;
    entry.identity = TextureIdentityForCompositedTile(tile);
    entry.uploadedGeneration = tile.generation;
    entry.width = textureDims.x;
    entry.height = textureDims.y;
#else
    liveIds.insert(tile.id);
    auto& entry = tileTextures_[tile.id];
    if (entry.texture == 0) {
      glGenTextures(1, &entry.texture);
      InitializeTexture(entry.texture);
    }

    // Re-upload only when the tile's generation advances OR the bitmap
    // dimensions changed (e.g. canvas resize landed a fresh rasterize at
    // a new size with the same generation seed — defensive).
    const CompositedTileTextureIdentity tileIdentity = TextureIdentityForCompositedTile(tile);
    const bool needsUpload = entry.identity != tileIdentity;
    if (needsUpload) {
      UploadBitmap(entry.texture, tile.bitmap, &entry.width, &entry.height);
      entry.identity = tileIdentity;
      entry.uploadedGeneration = tile.generation;
    }
#endif

    TileView view;
    view.texture = ToImTextureId(entry.texture);
    view.id = tile.id;
    view.kind = tile.kind;
    view.generation = tile.generation;
    view.bitmapDimsPx =
        !tile.bitmap.empty() ? tile.bitmap.dimensions : Vector2i(entry.width, entry.height);
    view.rasterCanvasSize = tile.rasterCanvasSize;
    view.canvasOffsetDoc = tile.canvasOffsetDoc;
    view.bitmapDimsDoc = tile.bitmapDimsDoc;
    view.dragTranslationDoc = tile.dragTranslationDoc;
    view.isDragTarget = tile.isDragTarget;
    tiles_.push_back(view);
  }

  // Evict textures whose tile id no longer appears.
  for (auto it = tileTextures_.begin(); it != tileTextures_.end();) {
    if (liveIds.find(it->first) == liveIds.end()) {
#ifndef DONNER_EDITOR_WGPU
      if (it->second.texture != 0) {
        glDeleteTextures(1, &it->second.texture);
      }
#else
      if (it->second.textureSnapshot != nullptr) {
        retiredSnapshots.push_back(
            RetireSnapshot(it->second.texture, std::move(it->second.textureSnapshot)));
      }
#endif
      it = tileTextures_.erase(it);
    } else {
      ++it;
    }
  }

  std::unordered_map<ImTextureID, std::string> liveTextureOwners;
  liveTextureOwners.reserve(tiles_.size());
  for (const TileView& tile : tiles_) {
    if (tile.texture == 0) {
      continue;
    }
    auto [ownerIt, inserted] = liveTextureOwners.emplace(tile.texture, tile.id);
    if (!inserted && ownerIt->second != tile.id) {
      ++duplicateLiveTextureCount_;
    }
  }

#ifdef DONNER_EDITOR_WGPU
  retireSnapshots(std::move(retiredSnapshots));
#endif
}

void GlTextureCache::advancePresentationFrame() {
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

void GlTextureCache::clearOverlay() {
#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch retiredSnapshots;
  if (overlayTextureSnapshot_ != nullptr) {
    retiredSnapshots.push_back(RetireSnapshot(overlayTexture_, std::move(overlayTextureSnapshot_)));
  }
  retireSnapshots(std::move(retiredSnapshots));
#else
  overlayTextureSnapshot_.reset();
#endif
  overlayTexture_ = 0;
  overlayWidth_ = 0;
  overlayHeight_ = 0;
}

void GlTextureCache::resetComposited() {
#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch retiredSnapshots;
  retiredSnapshots.reserve(tileTextures_.size());
  for (auto& [_, entry] : tileTextures_) {
    if (entry.textureSnapshot != nullptr) {
      retiredSnapshots.push_back(RetireSnapshot(entry.texture, std::move(entry.textureSnapshot)));
    }
  }
  retireSnapshots(std::move(retiredSnapshots));
#else
  for (auto& [_, entry] : tileTextures_) {
    if (entry.texture != 0) {
      glDeleteTextures(1, &entry.texture);
    }
  }
#endif
  tileTextures_.clear();
  tiles_.clear();
  metadataOnlyMissCount_ = 0;
  duplicateLiveTextureCount_ = 0;
}

ImTextureID GlTextureCache::overlayTexture() const {
  return ToImTextureId(overlayTexture_);
}

ImTextureID GlTextureCache::ToImTextureId(NativeTextureHandle texture) {
#ifdef DONNER_EDITOR_WGPU
  return texture;
#else
  return static_cast<ImTextureID>(texture);
#endif
}

#ifdef DONNER_EDITOR_WGPU
GlTextureCache::RetiredSnapshot GlTextureCache::RetireSnapshot(
    NativeTextureHandle texture, std::shared_ptr<const svg::RendererTextureSnapshot> snapshot) {
  return RetiredSnapshot{
      .texture = texture,
      .snapshot = std::move(snapshot),
  };
}

void GlTextureCache::releaseImGuiTexture(NativeTextureHandle texture) {
  if (texture == 0) {
    return;
  }

  ImGui_ImplWGPU_RemoveTexture(texture);
}

ImTextureID GlTextureCache::ToImTextureId(const svg::RendererTextureSnapshot* textureSnapshot) {
  if (textureSnapshot == nullptr ||
      textureSnapshot->backend() != svg::RendererTextureSnapshotBackend::Geode) {
    return 0;
  }
  const auto* geodeTexture = static_cast<const svg::RendererGeodeTextureSnapshot*>(textureSnapshot);
  const WGPUTextureView textureView = geodeTexture->textureView();
  return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(textureView));
}

void GlTextureCache::retireSnapshots(RetiredSnapshotBatch snapshots) {
  if (snapshots.empty()) {
    return;
  }

  pendingRetiredSnapshots_.insert(pendingRetiredSnapshots_.end(),
                                  std::make_move_iterator(snapshots.begin()),
                                  std::make_move_iterator(snapshots.end()));
}
#endif

#ifndef DONNER_EDITOR_WGPU
void GlTextureCache::UploadBitmap(GLuint texture, const svg::RendererBitmap& bitmap, int* outWidth,
                                  int* outHeight) {
  if (bitmap.empty()) {
    *outWidth = 0;
    *outHeight = 0;
    return;
  }

  glBindTexture(GL_TEXTURE_2D, texture);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(bitmap.rowBytes / 4u));
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bitmap.dimensions.x, bitmap.dimensions.y, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, bitmap.pixels.data());
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  *outWidth = bitmap.dimensions.x;
  *outHeight = bitmap.dimensions.y;
}

void GlTextureCache::InitializeTexture(GLuint texture) {
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
#endif

}  // namespace donner::editor
