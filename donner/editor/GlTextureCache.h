#pragma once
/// @file

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__EMSCRIPTEN__) && !defined(DONNER_EDITOR_WGPU)
#define GLFW_INCLUDE_ES3
#include <GLES3/gl3.h>
#elif !defined(DONNER_EDITOR_WGPU)
#include "glad/glad.h"
#endif

#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/ImGuiIncludes.h"

namespace donner::geode {
class GeodeDevice;
}  // namespace donner::geode

namespace donner::editor {

/// Stable identity for deciding whether a metadata-only composited tile can reuse an existing
/// presentation texture.
struct CompositedTileTextureIdentity {
  RenderResult::CompositedTile::Kind kind = RenderResult::CompositedTile::Kind::Segment;
  std::uint64_t generation = 0;
  Vector2i textureDimsPx = Vector2i::Zero();
  Vector2i rasterCanvasSize = Vector2i::Zero();

  friend bool operator==(const CompositedTileTextureIdentity& lhs,
                         const CompositedTileTextureIdentity& rhs) = default;
};

/**
 * Return the presentation texture identity carried by `tile`.
 *
 * @param tile Composited tile whose payload or metadata should be identified.
 */
[[nodiscard]] CompositedTileTextureIdentity TextureIdentityForCompositedTile(
    const RenderResult::CompositedTile& tile);

/**
 * Return true when a metadata-only `tile` can reuse `cachedIdentity`.
 *
 * @param cachedIdentity Identity recorded for the currently cached texture.
 * @param tile Incoming composited tile metadata.
 */
[[nodiscard]] bool TextureIdentityMatchesCompositedTile(
    const CompositedTileTextureIdentity& cachedIdentity, const RenderResult::CompositedTile& tile);

/**
 * Owns the GL textures the advanced editor uses for overlay and composited presentation.
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
  explicit GlTextureCache(std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice = nullptr);
  ~GlTextureCache();

  GlTextureCache(const GlTextureCache&) = delete;
  GlTextureCache& operator=(const GlTextureCache&) = delete;

  void initialize();

  void uploadOverlay(const svg::RendererBitmap& bitmap);
  /// Register an overlay texture snapshot without CPU readback/upload.
  void uploadOverlayTexture(std::shared_ptr<const svg::RendererTextureSnapshot> textureSnapshot);
  /// Upload the worker's tile snapshot into per-tile GL textures.
  /// Allocates/reuses one texture per tile id; bumps a per-tile
  /// upload-generation so identity uploads short-circuit; evicts
  /// textures whose tile is absent from the snapshot.
  void uploadComposited(const RenderResult::CompositedPreview& preview);

  /// Advance one presentation frame, retiring WGPU texture snapshots whose
  /// handles have aged past the backend's frames-in-flight window.
  void advancePresentationFrame();

  void clearOverlay();
  void resetComposited();

  [[nodiscard]] ImTextureID overlayTexture() const;

  [[nodiscard]] int overlayWidth() const { return overlayWidth_; }
  [[nodiscard]] int overlayHeight() const { return overlayHeight_; }

  /// One composite-tile entry as the presenter sees it: the GL
  /// texture handle (resolved from the upload cache) plus the
  /// geometry fields the presenter needs to blit in paint order.
  struct TileView {
    ImTextureID texture = 0;
    std::string id;
    RenderResult::CompositedTile::Kind kind = RenderResult::CompositedTile::Kind::Segment;
    Entity layerEntity = entt::null;
    std::uint64_t generation = 0;
    Vector2i bitmapDimsPx = Vector2i::Zero();
    Vector2i rasterCanvasSize = Vector2i::Zero();
    Vector2d canvasOffsetDoc = Vector2d::Zero();
    Vector2d bitmapDimsDoc = Vector2d::Zero();
    Vector2d dragTranslationDoc = Vector2d::Zero();
    Transform2d documentFromCachedDocument = Transform2d();
    bool metadataOnly = false;
    bool isDragTarget = false;
  };

  /// Paint-order tile view; empty when no composited preview has been
  /// uploaded yet (or the preview was cleared via `resetComposited`).
  [[nodiscard]] const std::vector<TileView>& tiles() const { return tiles_; }
  /// Number of metadata-only tiles skipped during the most recent composited
  /// upload because their cached texture identity was absent or stale.
  [[nodiscard]] int metadataOnlyMissCount() const { return metadataOnlyMissCount_; }
  /// Number of duplicate live texture handles found in the most recent
  /// composited upload across different tile ids.
  [[nodiscard]] int duplicateLiveTextureCount() const { return duplicateLiveTextureCount_; }

private:
#ifdef DONNER_EDITOR_WGPU
  using NativeTextureHandle = ImTextureID;
  struct WgpuUploadedTexture;
#else
  using NativeTextureHandle = GLuint;
#endif

  static ImTextureID ToImTextureId(NativeTextureHandle texture);
#ifdef DONNER_EDITOR_WGPU
  static ImTextureID ToImTextureId(const svg::RendererTextureSnapshot* textureSnapshot);
  std::shared_ptr<WgpuUploadedTexture> uploadBitmapToWgpu(const svg::RendererBitmap& bitmap);
#endif
#ifndef DONNER_EDITOR_WGPU
  static void UploadBitmap(GLuint texture, const svg::RendererBitmap& bitmap, int* outWidth,
                           int* outHeight);
  static void InitializeTexture(GLuint texture);
#endif

  struct CachedTextureEntry {
    NativeTextureHandle texture = 0;
    std::shared_ptr<const svg::RendererTextureSnapshot> textureSnapshot;
#ifdef DONNER_EDITOR_WGPU
    std::shared_ptr<WgpuUploadedTexture> uploadedTexture;
#endif
    CompositedTileTextureIdentity identity;
    std::uint64_t uploadedGeneration = 0;
    int width = 0;
    int height = 0;
  };

#ifdef DONNER_EDITOR_WGPU
  struct RetiredSnapshot {
    NativeTextureHandle texture = 0;
    std::shared_ptr<const svg::RendererTextureSnapshot> snapshot;
    std::shared_ptr<WgpuUploadedTexture> uploadedTexture;
  };

  using RetiredSnapshotBatch = std::vector<RetiredSnapshot>;

  static RetiredSnapshot RetireSnapshot(
      NativeTextureHandle texture, std::shared_ptr<const svg::RendererTextureSnapshot> snapshot);
  static void releaseImGuiTexture(NativeTextureHandle texture);

  void retireSnapshots(RetiredSnapshotBatch snapshots);

  std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice_;
#endif

  NativeTextureHandle overlayTexture_ = 0;
  std::shared_ptr<const svg::RendererTextureSnapshot> overlayTextureSnapshot_;
#ifdef DONNER_EDITOR_WGPU
  std::shared_ptr<WgpuUploadedTexture> overlayUploadedTexture_;
#endif

  int overlayWidth_ = 0;
  int overlayHeight_ = 0;

  /// Tile texture cache keyed on `CompositedTile::id`. Entries
  /// persist across frames so identical tiles re-use the same GL
  /// texture (only re-uploaded when their `generation` advances).
  std::unordered_map<std::string, CachedTextureEntry> tileTextures_;

  /// Paint-order view of the most recent `uploadComposited` call.
  /// Rebuilt every upload (cheap — N tiles, plain values).
  std::vector<TileView> tiles_;
  int metadataOnlyMissCount_ = 0;
  int duplicateLiveTextureCount_ = 0;

#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch pendingRetiredSnapshots_;
  std::deque<RetiredSnapshotBatch> retiredSnapshotFrames_;
#endif
};

}  // namespace donner::editor
