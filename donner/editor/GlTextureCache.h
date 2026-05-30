#pragma once
/// @file

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
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

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/FrameCostBreakdown.h"
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
  /// Payload dimensions in pixels. This is intentionally the valid content size, not the
  /// power-of-two backing allocation size.
  Vector2i textureDimsPx = Vector2i::Zero();
  Vector2i rasterCanvasSize = Vector2i::Zero();

  friend bool operator==(const CompositedTileTextureIdentity& lhs,
                         const CompositedTileTextureIdentity& rhs) = default;
};

/// Approximate resource footprint retained by the editor presentation texture cache.
struct PresentationResourceStats {
  /// Bytes retained by the current overlay texture.
  std::uint64_t overlayBytes = 0;
  /// Bytes retained by active composited tile textures.
  std::uint64_t activeTileBytes = 0;
  /// Bytes retained by the zoom-out overview tile textures.
  std::uint64_t overviewTileBytes = 0;
  /// Bytes queued for retirement at the next presentation-frame boundary.
  std::uint64_t pendingRetiredBytes = 0;
  /// Bytes held alive for already-advanced retirement frames.
  std::uint64_t agedRetiredBytes = 0;
  /// Total bytes directly tracked by this presentation cache.
  std::uint64_t totalTrackedBytes = 0;
  /// Highest observed \ref totalTrackedBytes for this cache instance.
  std::uint64_t peakTrackedBytes = 0;
  /// Number of active composited tile textures.
  int activeTileTextures = 0;
  /// Number of overview tile textures.
  int overviewTileTextures = 0;
  /// Number of textures queued for retirement at the next frame boundary.
  int pendingRetiredTextures = 0;
  /// Number of textures held in already-advanced retirement frames.
  int agedRetiredTextures = 0;
  /// Number of retirement frames currently retained.
  int retiredFrameCount = 0;
  /// Largest tracked backing allocation in pixels.
  Vector2i largestAllocationPx = Vector2i::Zero();
  /// Process-lifetime WebGPU texture-create count from the shared Geode device.
  std::uint64_t wgpuLifetimeTextureCreates = 0;
  /// Process-lifetime WebGPU buffer-create count from the shared Geode device.
  std::uint64_t wgpuLifetimeBufferCreates = 0;
};

/// Presentation coverage state for composited tile fallback diagnostics.
struct PresentationCoverageDiagnostics {
  /// True when the active composited tile set only covers a viewport-bounded raster.
  bool activeTilesViewportBounded = false;
  /// True when a retained unbounded overview is available under the active bounded tiles.
  bool overviewInfillAvailable = false;
  /// Document-space rectangle covered by active composited tiles.
  Box2d activeRasterDocumentRect;
  /// Document-space rectangle covered by retained overview tiles.
  Box2d overviewRasterDocumentRect;
  /// Output raster size for active composited tiles.
  Vector2i activeOutputSizePx = Vector2i::Zero();
  /// Output raster size for retained overview tiles.
  Vector2i overviewOutputSizePx = Vector2i::Zero();
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
 * Return the power-of-two backing texture dimensions for a payload.
 *
 * @param payloadDimensions Valid content dimensions in pixels.
 */
[[nodiscard]] Vector2i PowerOfTwoTextureDimensionsForPayload(const Vector2i& payloadDimensions);

/**
 * Return the max UV that samples only the payload inside a backing texture.
 *
 * @param payloadDimensions Valid content dimensions in pixels.
 * @param allocationDimensions Backing texture allocation dimensions in pixels.
 */
[[nodiscard]] Vector2d TextureUvBottomRightForPayload(const Vector2i& payloadDimensions,
                                                      const Vector2i& allocationDimensions);

/**
 * Return the byte size of a CPU bitmap payload as submitted to the presentation cache.
 *
 * @param bitmap CPU renderer bitmap.
 */
[[nodiscard]] std::uint64_t BitmapPayloadBytes(const svg::RendererBitmap& bitmap);

/**
 * Return the approximate byte size of a backend texture payload, assuming RGBA8 storage.
 *
 * @param dimensions Texture dimensions in pixels.
 */
[[nodiscard]] std::uint64_t TexturePayloadBytes(const Vector2i& dimensions);

/**
 * Compute upload-cost counters for a composited preview without touching GL/WGPU state.
 *
 * @param preview Composited preview to inspect.
 */
[[nodiscard]] FrameCostBreakdown::CompositedUpload CostForCompositedPreviewUpload(
    const RenderResult::CompositedPreview& preview);

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
  void uploadComposited(const RenderResult::CompositedPreview& preview,
                        std::optional<EditorRasterViewport> rasterViewport = std::nullopt);
  /// Upload a full-document overview preview without replacing active viewport-bounded tiles.
  void uploadCompositedOverview(const RenderResult::CompositedPreview& preview,
                                const EditorRasterViewport& rasterViewport);

  /// Advance one presentation frame, retiring WGPU texture snapshots whose
  /// handles have aged past the backend's frames-in-flight window.
  void advancePresentationFrame();

  void clearOverlay();
  void resetComposited();

  [[nodiscard]] ImTextureID overlayTexture() const;

  [[nodiscard]] int overlayWidth() const { return overlayWidth_; }
  [[nodiscard]] int overlayHeight() const { return overlayHeight_; }
  /// Bottom-right UV for sampling the valid overlay payload from its backing texture.
  [[nodiscard]] const Vector2d& overlayUvBottomRight() const { return overlayUvBottomRight_; }
  /// Screen rect for viewport-space overlay textures.
  ///
  /// Nullopt means the overlay texture uses the legacy document-viewBox
  /// placement path.
  [[nodiscard]] const std::optional<Box2d>& overlayScreenRect() const { return overlayScreenRect_; }
  /// Record the screen-space placement for the currently uploaded overlay.
  ///
  /// @param screenRect ImGui screen rect covered by the overlay texture, or nullopt to use the
  ///   legacy document-viewBox placement path.
  void setOverlayScreenRect(std::optional<Box2d> screenRect);

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
    Vector2d uvBottomRight = Vector2d(1.0, 1.0);
    Transform2d documentFromCachedDocument = Transform2d();
    bool metadataOnly = false;
    bool isDragTarget = false;
  };

  /// Paint-order tile view; empty when no composited preview has been
  /// uploaded yet (or the preview was cleared via `resetComposited`).
  [[nodiscard]] const std::vector<TileView>& tiles() const { return tiles_; }
  /// Last retained unbounded full-document tile set, drawn underneath
  /// viewport-bounded tiles as a coherent zoom-out fallback.
  [[nodiscard]] const std::vector<TileView>& overviewTiles() const { return overviewTiles_; }
  /// True when the active tile set was rendered from a viewport-bounded raster target.
  [[nodiscard]] bool activeTilesViewportBounded() const { return activeTilesViewportBounded_; }
  /// Number of metadata-only tiles skipped during the most recent composited
  /// upload because their cached texture identity was absent or stale.
  [[nodiscard]] int metadataOnlyMissCount() const { return metadataOnlyMissCount_; }
  /// Number of duplicate live texture handles found in the most recent
  /// composited upload across different tile ids.
  [[nodiscard]] int duplicateLiveTextureCount() const { return duplicateLiveTextureCount_; }
  /// Cost counters for the most recent composited upload.
  [[nodiscard]] const FrameCostBreakdown::CompositedUpload& lastCompositedUploadCost() const {
    return lastCompositedUploadCost_;
  }
  /// Cost counters for the most recent overlay upload or clear.
  [[nodiscard]] const FrameCostBreakdown::Overlay& lastOverlayUploadCost() const {
    return lastOverlayUploadCost_;
  }
  /// Resource counters for the textures currently retained by this cache.
  [[nodiscard]] PresentationResourceStats presentationResourceStats() const;
  /// Coverage counters for active bounded tiles and retained overview infill.
  [[nodiscard]] PresentationCoverageDiagnostics coverageDiagnostics() const;

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
  std::shared_ptr<WgpuUploadedTexture> uploadBitmapToWgpu(
      const svg::RendererBitmap& bitmap,
      const std::shared_ptr<WgpuUploadedTexture>& reusableTexture = nullptr);
#endif
#ifndef DONNER_EDITOR_WGPU
  static void UploadBitmap(GLuint texture, const svg::RendererBitmap& bitmap, int* outWidth,
                           int* outHeight, int* outAllocatedWidth, int* outAllocatedHeight,
                           Vector2d* outUvBottomRight);
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
    int allocatedWidth = 0;
    int allocatedHeight = 0;
    Vector2d uvBottomRight = Vector2d(1.0, 1.0);
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
  int overlayAllocatedWidth_ = 0;
  int overlayAllocatedHeight_ = 0;
  Vector2d overlayUvBottomRight_ = Vector2d(1.0, 1.0);
  std::optional<Box2d> overlayScreenRect_;

  /// Tile texture cache keyed on `CompositedTile::id`. Entries
  /// persist across frames so identical tiles re-use the same GL
  /// texture (only re-uploaded when their `generation` advances).
  std::unordered_map<std::string, CachedTextureEntry> tileTextures_;
  /// Separately-owned copy of the last unbounded tile set. Active
  /// high-zoom uploads may reuse the same tile ids at a smaller raster
  /// size, so overview textures cannot share the active cache entries.
  std::unordered_map<std::string, CachedTextureEntry> overviewTileTextures_;

  /// Paint-order view of the most recent `uploadComposited` call.
  /// Rebuilt every upload (cheap — N tiles, plain values).
  std::vector<TileView> tiles_;
  /// Paint-order view of `overviewTileTextures_`.
  std::vector<TileView> overviewTiles_;
  bool activeTilesViewportBounded_ = false;
  Box2d activeRasterDocumentRect_;
  Box2d overviewRasterDocumentRect_;
  Vector2i activeOutputSizePx_ = Vector2i::Zero();
  Vector2i overviewOutputSizePx_ = Vector2i::Zero();
  int metadataOnlyMissCount_ = 0;
  int duplicateLiveTextureCount_ = 0;
  FrameCostBreakdown::CompositedUpload lastCompositedUploadCost_;
  FrameCostBreakdown::Overlay lastOverlayUploadCost_;
  mutable std::uint64_t peakTrackedResourceBytes_ = 0;

#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch pendingRetiredSnapshots_;
  std::deque<RetiredSnapshotBatch> retiredSnapshotFrames_;
#endif
};

}  // namespace donner::editor
