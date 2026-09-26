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

// The browser tier is Geode-only, so the OpenGL presentation path is desktop-only.
#ifndef DONNER_EDITOR_WGPU
#include "glad/glad.h"
#endif

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/FrameCostBreakdown.h"
#include "donner/editor/ImGuiIncludes.h"
#ifdef DONNER_EDITOR_WGPU
#include "donner/editor/gui/UiTextureRegistration.h"
#endif

namespace donner::geode {
class GeodeDevice;
}  // namespace donner::geode

namespace donner::svg {
class RendererGeodeTextureSnapshot;
}  // namespace donner::svg

namespace donner::editor {

/// Stable identity for deciding whether a metadata-only composited tile can reuse an existing
/// presentation texture.
struct CompositedTileTextureIdentity {
  RenderResult::CompositedTile::Kind kind =
      RenderResult::CompositedTile::Kind::Segment;  //!< Kind of tile whose payload was cached.
  std::uint64_t generation = 0;                     //!< Generation of the cached tile payload.
  Vector2i textureDimsPx = Vector2i::Zero();        //!< Valid payload size, not backing allocation.
  Vector2i rasterCanvasSize = Vector2i::Zero();     //!< Canvas size used to render this tile.

  /// Compare all fields used for texture reuse.
  /// @param lhs First identity.
  /// @param rhs Second identity.
  friend bool operator==(const CompositedTileTextureIdentity& lhs,
                         const CompositedTileTextureIdentity& rhs) = default;
};

/// Approximate resource footprint retained by the editor presentation texture cache.
struct PresentationResourceStats {
  /// Bytes retained by an external explicit document-composite target.
  std::uint64_t documentCompositeBytes = 0;
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
 * Retains editor presentation textures for composited tiles and Layers thumbnails.
 *
 * Stable tile ids and generations let metadata-only previews reuse prior payloads. Geode builds
 * register renderer snapshots with the UI texture registry and upload CPU bitmaps into runtime
 * textures; the desktop OpenGL path owns GL textures. Removed tiles and superseded GPU snapshots
 * are released after their presentation lifetime, while thumbnail entries are retained by row key.
 */
class GlTextureCache {
public:
  /// Construct a cache for the selected presentation backend.
  /// @param geodeDevice Shared Geode context, or null for the OpenGL path.
  explicit GlTextureCache(std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice = nullptr);
  ~GlTextureCache();

  GlTextureCache(const GlTextureCache&) = delete;
  GlTextureCache& operator=(const GlTextureCache&) = delete;

  /// Compatibility initialization hook; presentation resources are allocated lazily.
  void initialize();

  /// Register or upload each worker tile for the selected presentation backend.
  /// Reuse unchanged tile identities and evict payloads absent from the new snapshot.
  /// @param preview Paint-ordered worker tiles and metadata.
  /// @param rasterViewport Raster coverage represented by the preview, when known.
  void uploadComposited(const RenderResult::CompositedPreview& preview,
                        std::optional<EditorRasterViewport> rasterViewport = std::nullopt);
  /// Upload a full-document overview preview without replacing active viewport-bounded tiles.
  void uploadCompositedOverview(const RenderResult::CompositedPreview& preview,
                                const EditorRasterViewport& rasterViewport);

  /// Advance one presentation frame, retiring WGPU texture snapshots whose
  /// handles have aged past the backend's frames-in-flight window.
  void advancePresentationFrame();

  /// Clear active and overview composited tiles while retaining thumbnail entries.
  void resetComposited();

  /// ImGui texture handle and UV range for valid thumbnail content.
  struct ThumbnailTextureView {
    ImTextureID texture = 0;                      ///< ImGui texture handle.
    Vector2d uvBottomRight = Vector2d(1.0, 1.0);  ///< Bottom-right valid payload UV.
  };

  /// Upload a Donner-rendered Layers thumbnail, reusing the row's unchanged texture.
  /// The bitmap is keyed by row id and content fingerprint; ImGui only blits its pixels.
  /// @param key Stable id of the Layers row.
  /// @param bitmap Donner-rendered RGBA thumbnail bitmap.
  /// @return Handle and valid UV range, or an empty view for an empty bitmap. A failed Geode
  /// bitmap upload keeps the prior view when one exists; a subsequent UI texture-registration
  /// failure returns a zero texture handle.
  ThumbnailTextureView uploadThumbnail(std::uint64_t key, const svg::RendererBitmap& bitmap);

  /// Retain a renderer-owned GPU snapshot as a Layers thumbnail on Geode builds.
  /// The OpenGL path returns an empty view; use `uploadThumbnail` there.
  /// @param key Stable id of the Layers row.
  /// @param textureSnapshot Renderer snapshot to retain until replacement or eviction.
  /// @return Handle and valid UV range, or an empty view if registration fails.
  ThumbnailTextureView retainThumbnailTextureSnapshot(
      std::uint64_t key, std::shared_ptr<const svg::RendererTextureSnapshot> textureSnapshot);

  /// Report bytes retained by the non-WGPU explicit document compositor.
  void setDocumentCompositeBytes(std::uint64_t bytes) { documentCompositeBytes_ = bytes; }
  /// Evict every cached thumbnail texture whose key is not in @p liveKeys,
  /// freeing the backing GL/WGPU texture. Called after each Layers-panel render
  /// so thumbnails for removed rows do not leak across refreshes.
  ///
  /// @param liveKeys Stable ids of the rows currently shown by the panel.
  void retainThumbnailsOnly(const std::vector<std::uint64_t>& liveKeys);

  /// Number of thumbnail textures currently retained (testing/diagnostics).
  [[nodiscard]] std::size_t thumbnailTextureCount() const { return thumbnailTextures_.size(); }

  /// One cached tile's ImGui texture handle and paint-order geometry.
  struct TileView {
    ImTextureID texture =
        0;           //!< ImGui handle for this tile, or zero when no cached payload can be reused.
    std::string id;  //!< Stable composited tile identifier.
    RenderResult::CompositedTile::Kind kind =
        RenderResult::CompositedTile::Kind::Segment;  //!< Composited tile category.
    Entity layerEntity = entt::null;               //!< Layer entity when this is a promoted layer.
    std::uint64_t generation = 0;                  //!< Generation of the tile payload.
    Vector2i bitmapDimsPx = Vector2i::Zero();      //!< Valid texture payload dimensions in pixels.
    Vector2i rasterCanvasSize = Vector2i::Zero();  //!< Raster canvas size that produced this tile.
    Vector2d canvasOffsetDoc = Vector2d::Zero();   //!< Document-space tile origin.
    Vector2d bitmapDimsDoc = Vector2d::Zero();     //!< Document-space tile dimensions.
    Vector2d dragTranslationDoc =
        Vector2d::Zero();  //!< Document-space drag offset for presentation.
    std::shared_ptr<const svg::RendererTextureSnapshot>
        textureSnapshot;                          //!< Retained GPU snapshot backing a Geode tile.
    Vector2d uvBottomRight = Vector2d(1.0, 1.0);  //!< Bottom-right UV of valid texture content.
    Transform2d documentFromCachedDocument =
        Transform2d();          //!< Transform from cached-document to current-document space.
    bool metadataOnly = false;  //!< True when the tile carries no new texture payload.
    bool isDragTarget = false;  //!< Whether this tile is the active drag target.
  };

  /// Paint-order tile view; metadata-only direct-surface entries have a zero texture handle.
  /// Empty when no composited preview has been uploaded yet (or the preview was cleared via
  /// `resetComposited`).
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
  /// Resource counters for the textures currently retained by this cache.
  [[nodiscard]] PresentationResourceStats presentationResourceStats() const;
  /// Coverage counters for active bounded tiles and retained overview infill.
  [[nodiscard]] PresentationCoverageDiagnostics coverageDiagnostics() const;

private:
#ifdef DONNER_EDITOR_WGPU
  using NativeTextureHandle = ImTextureID;
#else
  using NativeTextureHandle = GLuint;
#endif

  static ImTextureID ToImTextureId(NativeTextureHandle texture);
#ifdef DONNER_EDITOR_WGPU
  /// Registers \p snapshot as a UI texture and retains the handles backing it until the
  /// registration is retired. Returns zero when it cannot be registered.
  /// @param snapshot Snapshot to register.
  NativeTextureHandle registerSnapshotTexture(const svg::RendererTextureSnapshot& snapshot);

  /// Upload a CPU bitmap into a runtime texture owned by the returned snapshot, or null when the
  /// payload is invalid or the runtime refuses the upload.
  ///
  /// Every upload allocates its own texture rather than overwriting one this cache has already
  /// published: the runtime write is chunked, so an in-place replacement that is refused part way
  /// through would leave the presented allocation holding a mix of the old and new payloads. The
  /// superseded allocation is handed to \ref retireSnapshots and released once its presentation
  /// frames have elapsed, the same lifetime the cache already gives backend-produced snapshots.
  /// @param bitmap Payload to upload.
  std::shared_ptr<svg::RendererGeodeTextureSnapshot> uploadBitmapToWgpu(
      const svg::RendererBitmap& bitmap);
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
    /// Backing allocation the retired texture still holds, which can exceed the snapshot's
    /// content extent.
    Vector2i allocationDimensions = Vector2i::Zero();
  };

  using RetiredSnapshotBatch = std::vector<RetiredSnapshot>;

  /// Retires \p texture's registration and drops the handles that backed it.
  /// @param texture Identifier to retire.
  [[gnu::noinline]] void releaseImGuiTexture(NativeTextureHandle texture);

  void retireSnapshots(RetiredSnapshotBatch snapshots);

  std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice_;
  /// Handles backing each live registration, dropped when that registration is retired.
  std::unordered_map<ImTextureID, UiTextureBacking> registeredBackings_;
#endif

  /// Tile payload cache keyed on `CompositedTile::id`; unchanged identities reuse their
  /// GL texture or Geode UI registration across frames.
  std::unordered_map<std::string, CachedTextureEntry> tileTextures_;
  /// Separately-owned copy of the last unbounded tile set. Active
  /// high-zoom uploads may reuse the same tile ids at a smaller raster
  /// size, so overview textures cannot share the active cache entries.
  std::unordered_map<std::string, CachedTextureEntry> overviewTileTextures_;

  /// Layers-panel thumbnail texture cache keyed on the row stable id. Each
  /// entry owns one GL/WGPU texture, reuploaded only when the row's thumbnail
  /// bitmap changes (tracked via `CachedTextureEntry::uploadedGeneration`
  /// holding a content fingerprint plus the cached width/height). Evicted by
  /// `retainThumbnailsOnly` when a row leaves the panel.
  std::unordered_map<std::uint64_t, CachedTextureEntry> thumbnailTextures_;

  /// Paint-order view of the most recent `uploadComposited` call.
  /// Rebuilt every upload (cheap - N tiles, plain values).
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
  std::uint64_t documentCompositeBytes_ = 0;
  mutable std::uint64_t peakTrackedResourceBytes_ = 0;

#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch pendingRetiredSnapshots_;
  std::deque<RetiredSnapshotBatch> retiredSnapshotFrames_;
#endif
};

}  // namespace donner::editor
