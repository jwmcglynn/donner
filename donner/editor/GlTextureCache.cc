#include "donner/editor/GlTextureCache.h"

#include "donner/editor/TracyWrapper.h"

namespace donner::editor {

GlTextureCache::~GlTextureCache() {
  if (flatTexture_ != 0) {
    glDeleteTextures(1, &flatTexture_);
  }
  if (overlayTexture_ != 0) {
    glDeleteTextures(1, &overlayTexture_);
  }
  for (auto& [_, entry] : tileTextures_) {
    if (entry.texture != 0) {
      glDeleteTextures(1, &entry.texture);
    }
  }
}

void GlTextureCache::initialize() {
  if (flatTexture_ == 0) {
    glGenTextures(1, &flatTexture_);
    InitializeTexture(flatTexture_);
  }
  if (overlayTexture_ == 0) {
    glGenTextures(1, &overlayTexture_);
    InitializeTexture(overlayTexture_);
  }
}

void GlTextureCache::uploadFlat(const svg::RendererBitmap& bitmap) {
  ZoneScopedN("GlTextureCache::uploadFlat");
  UploadBitmap(flatTexture_, bitmap, &flatWidth_, &flatHeight_);
}

void GlTextureCache::uploadOverlay(const svg::RendererBitmap& bitmap) {
  ZoneScopedN("GlTextureCache::uploadOverlay");
  UploadBitmap(overlayTexture_, bitmap, &overlayWidth_, &overlayHeight_);
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
    if (tile.bitmap.empty()) {
      continue;
    }
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

    TileView view;
    view.texture = entry.texture;
    view.bitmapDimsPx = tile.bitmap.dimensions;
    view.canvasOffsetDoc = tile.canvasOffsetDoc;
    view.bitmapDimsDoc = tile.bitmapDimsDoc;
    view.dragTranslationDoc = tile.dragTranslationDoc;
    view.isDragTarget = tile.isDragTarget;
    tiles_.push_back(view);
  }

  // Evict textures whose tile id no longer appears.
  for (auto it = tileTextures_.begin(); it != tileTextures_.end();) {
    if (liveIds.find(it->first) == liveIds.end()) {
      if (it->second.texture != 0) {
        glDeleteTextures(1, &it->second.texture);
      }
      it = tileTextures_.erase(it);
    } else {
      ++it;
    }
  }
}

void GlTextureCache::clearOverlay() {
  overlayWidth_ = 0;
  overlayHeight_ = 0;
}

void GlTextureCache::resetComposited() {
  for (auto& [_, entry] : tileTextures_) {
    if (entry.texture != 0) {
      glDeleteTextures(1, &entry.texture);
    }
  }
  tileTextures_.clear();
  tiles_.clear();
}

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

}  // namespace donner::editor
