#pragma once
/// @file

#include <cstdint>

#ifdef __EMSCRIPTEN__
#define GLFW_INCLUDE_ES3
#include <GLES3/gl3.h>
#else
#include "glad/glad.h"
#endif

#include "donner/editor/AsyncRenderer.h"

namespace donner::editor {

/**
 * Owns the GL textures the advanced editor uses for flat, overlay, and composited presentation.
 *
 * The cache is intentionally dumb: callers decide *when* textures should update; this class only
 * owns the texture names, uploads pixel buffers, and tracks the currently-valid dimensions.
 */
class GlTextureCache {
public:
  GlTextureCache() = default;
  ~GlTextureCache();

  GlTextureCache(const GlTextureCache&) = delete;
  GlTextureCache& operator=(const GlTextureCache&) = delete;

  void initialize();

  void uploadFlat(const svg::RendererBitmap& bitmap);
  void uploadOverlay(const svg::RendererBitmap& bitmap);
  void uploadComposited(const RenderResult::CompositedPreview& preview);

  void clearOverlay();
  void resetComposited();

  [[nodiscard]] GLuint flatTexture() const { return flatTexture_; }
  [[nodiscard]] GLuint overlayTexture() const { return overlayTexture_; }
  [[nodiscard]] GLuint backgroundTexture() const { return backgroundTexture_; }
  [[nodiscard]] GLuint promotedTexture() const { return promotedTexture_; }
  [[nodiscard]] GLuint foregroundTexture() const { return foregroundTexture_; }

  [[nodiscard]] int flatWidth() const { return flatWidth_; }
  [[nodiscard]] int flatHeight() const { return flatHeight_; }
  [[nodiscard]] int overlayWidth() const { return overlayWidth_; }
  [[nodiscard]] int overlayHeight() const { return overlayHeight_; }
  [[nodiscard]] int backgroundWidth() const { return backgroundWidth_; }
  [[nodiscard]] int backgroundHeight() const { return backgroundHeight_; }
  [[nodiscard]] int promotedWidth() const { return promotedWidth_; }
  [[nodiscard]] int promotedHeight() const { return promotedHeight_; }
  [[nodiscard]] int foregroundWidth() const { return foregroundWidth_; }
  [[nodiscard]] int foregroundHeight() const { return foregroundHeight_; }

private:
  static void UploadBitmap(GLuint texture, const svg::RendererBitmap& bitmap, int* outWidth,
                           int* outHeight);
  static void InitializeTexture(GLuint texture);

  GLuint flatTexture_ = 0;
  GLuint overlayTexture_ = 0;
  GLuint backgroundTexture_ = 0;
  GLuint promotedTexture_ = 0;
  GLuint foregroundTexture_ = 0;

  int flatWidth_ = 0;
  int flatHeight_ = 0;
  int overlayWidth_ = 0;
  int overlayHeight_ = 0;
  int backgroundWidth_ = 0;
  int backgroundHeight_ = 0;
  int promotedWidth_ = 0;
  int promotedHeight_ = 0;
  int foregroundWidth_ = 0;
  int foregroundHeight_ = 0;
};

}  // namespace donner::editor
