#include "donner/editor/GlTextureCache.h"

#include "donner/editor/TracyWrapper.h"
#ifdef DONNER_EDITOR_WGPU
#include "donner/svg/renderer/RendererGeode.h"
#endif

namespace donner::editor {

GlTextureCache::~GlTextureCache() {
#ifndef DONNER_EDITOR_WGPU
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
  overlayTextureSnapshot_ = std::move(textureSnapshot);
  overlayTexture_ = ToImTextureId(overlayTextureSnapshot_.get());
  if (overlayTextureSnapshot_ != nullptr && overlayTexture_ != 0) {
    const Vector2i dims = overlayTextureSnapshot_->dimensions();
    overlayWidth_ = dims.x;
    overlayHeight_ = dims.y;
  } else {
    clearOverlay();
  }
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

  for (const auto& tile : preview.tiles) {
    // Partial tile snapshots can carry metadata without a pixel payload.
    // Preserve the already-uploaded texture for that tile id and update only
    // its presentation geometry.
    if (tile.bitmap.empty() && tile.textureSnapshot == nullptr) {
      auto it = tileTextures_.find(tile.id);
      if (it == tileTextures_.end()) {
        continue;
      }
      liveIds.insert(tile.id);
      TileView view;
      view.texture = ToImTextureId(it->second.texture);
      view.bitmapDimsPx = Vector2i(it->second.width, it->second.height);
      view.canvasOffsetDoc = tile.canvasOffsetDoc;
      view.bitmapDimsDoc = tile.bitmapDimsDoc;
      view.dragTranslationDoc = tile.dragTranslationDoc;
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
    entry.texture = textureId;
    entry.textureSnapshot = tile.textureSnapshot;
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
    const bool needsUpload = entry.uploadedGeneration != tile.generation ||
                             entry.width != tile.bitmap.dimensions.x ||
                             entry.height != tile.bitmap.dimensions.y;
    if (needsUpload) {
      UploadBitmap(entry.texture, tile.bitmap, &entry.width, &entry.height);
      entry.uploadedGeneration = tile.generation;
    }
#endif

    TileView view;
    view.texture = ToImTextureId(entry.texture);
    view.bitmapDimsPx =
        !tile.bitmap.empty() ? tile.bitmap.dimensions : Vector2i(entry.width, entry.height);
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
#endif
      it = tileTextures_.erase(it);
    } else {
      ++it;
    }
  }
}

void GlTextureCache::clearOverlay() {
  overlayTexture_ = 0;
  overlayTextureSnapshot_.reset();
  overlayWidth_ = 0;
  overlayHeight_ = 0;
}

void GlTextureCache::resetComposited() {
#ifndef DONNER_EDITOR_WGPU
  for (auto& [_, entry] : tileTextures_) {
    if (entry.texture != 0) {
      glDeleteTextures(1, &entry.texture);
    }
  }
#endif
  tileTextures_.clear();
  tiles_.clear();
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
ImTextureID GlTextureCache::ToImTextureId(const svg::RendererTextureSnapshot* textureSnapshot) {
  if (textureSnapshot == nullptr ||
      textureSnapshot->backend() != svg::RendererTextureSnapshotBackend::Geode) {
    return 0;
  }
  const auto* geodeTexture = static_cast<const svg::RendererGeodeTextureSnapshot*>(textureSnapshot);
  const WGPUTextureView textureView = geodeTexture->textureView();
  return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(textureView));
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
