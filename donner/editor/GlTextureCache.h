#pragma once
/// @file

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef __EMSCRIPTEN__
#define GLFW_INCLUDE_ES3
#include <GLES3/gl3.h>
#else
#include "glad/glad.h"
#endif

#include "donner/base/Vector2.h"
#include "donner/editor/AsyncRenderer.h"

namespace donner::editor {

/**
 * Owns the GL textures the advanced editor uses for flat, overlay, and composited presentation.
 *
 * The cache is intentionally dumb: callers decide *when* textures should update; this class only
 * owns the texture names, uploads pixel buffers, and tracks the currently-valid dimensions.
 *
 * Composited preview rendering follows design doc 0033 §M2C: instead of three named bg/promoted/fg
 * textures, the cache owns one GL texture per `CompositedTile` (keyed on the tile id), allocated
 * lazily on first upload and freed when the tile leaves the snapshot. The editor's
 * `RenderPanePresenter` iterates `tiles()` in paint order and blits each tile at its
 * `(canvasOffsetDoc + dragTranslationDoc) * pixelsPerDocUnit` origin.
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
  /// Upload the worker's tile snapshot into per-tile GL textures.
  /// Allocates/reuses one texture per tile id; bumps a per-tile
  /// upload-generation so identity uploads short-circuit; evicts
  /// textures whose tile is absent from the snapshot.
  void uploadComposited(const RenderResult::CompositedPreview& preview);

  void clearOverlay();
  void resetComposited();

  [[nodiscard]] GLuint flatTexture() const { return flatTexture_; }
  [[nodiscard]] GLuint overlayTexture() const { return overlayTexture_; }

  [[nodiscard]] int flatWidth() const { return flatWidth_; }
  [[nodiscard]] int flatHeight() const { return flatHeight_; }
  [[nodiscard]] int overlayWidth() const { return overlayWidth_; }
  [[nodiscard]] int overlayHeight() const { return overlayHeight_; }

  /// One composite-tile entry as the presenter sees it: the GL
  /// texture handle (resolved from the upload cache) plus the
  /// geometry fields the presenter needs to blit in paint order.
  struct TileView {
    GLuint texture = 0;
    Vector2i bitmapDimsPx = Vector2i::Zero();
    Vector2d canvasOffsetDoc = Vector2d::Zero();
    Vector2d bitmapDimsDoc = Vector2d::Zero();
    Vector2d dragTranslationDoc = Vector2d::Zero();
    bool isDragTarget = false;
  };

  /// Paint-order tile view; empty when no composited preview has been
  /// uploaded yet (or the preview was cleared via `resetComposited`).
  [[nodiscard]] const std::vector<TileView>& tiles() const { return tiles_; }

private:
  static void UploadBitmap(GLuint texture, const svg::RendererBitmap& bitmap, int* outWidth,
                           int* outHeight);
  static void InitializeTexture(GLuint texture);

  struct CachedTextureEntry {
    GLuint texture = 0;
    std::uint64_t uploadedGeneration = 0;
    int width = 0;
    int height = 0;
  };

  GLuint flatTexture_ = 0;
  GLuint overlayTexture_ = 0;

  int flatWidth_ = 0;
  int flatHeight_ = 0;
  int overlayWidth_ = 0;
  int overlayHeight_ = 0;

  /// Tile texture cache keyed on `CompositedTile::id`. Entries
  /// persist across frames so identical tiles re-use the same GL
  /// texture (only re-uploaded when their `generation` advances).
  std::unordered_map<std::string, CachedTextureEntry> tileTextures_;

  /// Paint-order view of the most recent `uploadComposited` call.
  /// Rebuilt every upload (cheap — N tiles, plain values).
  std::vector<TileView> tiles_;
};

}  // namespace donner::editor
