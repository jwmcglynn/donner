#include "donner/editor/GlTextureCache.h"

namespace donner::editor {

GlTextureCache::~GlTextureCache() {
  if (flatTexture_ != 0) {
    glDeleteTextures(1, &flatTexture_);
  }
  if (overlayTexture_ != 0) {
    glDeleteTextures(1, &overlayTexture_);
  }
  if (backgroundTexture_ != 0) {
    glDeleteTextures(1, &backgroundTexture_);
  }
  if (promotedTexture_ != 0) {
    glDeleteTextures(1, &promotedTexture_);
  }
  if (foregroundTexture_ != 0) {
    glDeleteTextures(1, &foregroundTexture_);
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
  if (backgroundTexture_ == 0) {
    glGenTextures(1, &backgroundTexture_);
    InitializeTexture(backgroundTexture_);
  }
  if (promotedTexture_ == 0) {
    glGenTextures(1, &promotedTexture_);
    InitializeTexture(promotedTexture_);
  }
  if (foregroundTexture_ == 0) {
    glGenTextures(1, &foregroundTexture_);
    InitializeTexture(foregroundTexture_);
  }
}

void GlTextureCache::uploadFlat(const svg::RendererBitmap& bitmap) {
  UploadBitmap(flatTexture_, bitmap, &flatWidth_, &flatHeight_);
}

void GlTextureCache::uploadOverlay(const svg::RendererBitmap& bitmap) {
  UploadBitmap(overlayTexture_, bitmap, &overlayWidth_, &overlayHeight_);
}

void GlTextureCache::uploadComposited(const RenderResult::CompositedPreview& preview) {
  UploadBitmap(backgroundTexture_, preview.backgroundBitmap, &backgroundWidth_, &backgroundHeight_);
  UploadBitmap(promotedTexture_, preview.promotedBitmap, &promotedWidth_, &promotedHeight_);
  UploadBitmap(foregroundTexture_, preview.foregroundBitmap, &foregroundWidth_, &foregroundHeight_);
}

void GlTextureCache::clearOverlay() {
  overlayWidth_ = 0;
  overlayHeight_ = 0;
}

void GlTextureCache::resetComposited() {
  backgroundWidth_ = 0;
  backgroundHeight_ = 0;
  promotedWidth_ = 0;
  promotedHeight_ = 0;
  foregroundWidth_ = 0;
  foregroundHeight_ = 0;
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
