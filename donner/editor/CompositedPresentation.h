#pragma once
/// @file

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <ostream>
#include <variant>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/MathUtils.h"
#include "donner/base/Vector2.h"
#include "donner/editor/SelectTool.h"

namespace donner::editor {

/// Tracks composited presentation state across selection, drag, and release.
class CompositedPresentation {
public:
  /// Closed presentation phase exposed through diagnostics.
  enum class Phase {
    NoCache,
    Cached,
    SettlingForRender,
    WaitingForChromeRefresh,
  };

  /// Immutable-by-copy presentation diagnostics for tests and MCP reporting.
  struct DiagnosticsSnapshot {
    Phase phase = Phase::NoCache;
    bool hasCachedTextures = false;
    Entity cachedEntity = entt::null;
    std::uint64_t cachedVersion = 0;
    Vector2i cachedCanvasSize = Vector2i::Zero();
    std::optional<SelectTool::ActiveDragPreview> settlingPreview;
    bool waitingForFullRender = false;
    std::uint64_t settlingTargetVersion = 0;
    bool waitingForChromeRefresh = false;
    std::uint64_t chromeRefreshTargetVersion = 0;
  };

private:
  struct CachedTextures {
    Entity entity = entt::null;
    std::uint64_t version = 0;
    Vector2i canvasSize = Vector2i::Zero();
    std::optional<SelectTool::ActiveDragPreview> representedPreview;
  };

  struct NoCache {};

  struct Cached {
    CachedTextures cache;
  };

  struct SettlingForRender {
    std::optional<CachedTextures> cache;
    SelectTool::ActiveDragPreview preview;
    std::uint64_t targetVersion = 0;
  };

  struct WaitingForChromeRefresh {
    CachedTextures cache;
    SelectTool::ActiveDragPreview preview;
    std::uint64_t targetVersion = 0;
  };

  using State = std::variant<NoCache, Cached, SettlingForRender, WaitingForChromeRefresh>;

public:
  /// Return a copied diagnostic snapshot of the current presentation state.
  [[nodiscard]] DiagnosticsSnapshot diagnostics() const {
    DiagnosticsSnapshot snapshot;

    if (const auto* cached = std::get_if<Cached>(&state_)) {
      snapshot.phase = Phase::Cached;
      FillCacheDiagnostics(cached->cache, &snapshot);
      return snapshot;
    }

    if (const auto* settling = std::get_if<SettlingForRender>(&state_)) {
      snapshot.phase = Phase::SettlingForRender;
      if (settling->cache.has_value()) {
        FillCacheDiagnostics(*settling->cache, &snapshot);
      }
      snapshot.settlingPreview = settling->preview;
      snapshot.waitingForFullRender = true;
      snapshot.settlingTargetVersion = settling->targetVersion;
      return snapshot;
    }

    if (const auto* waiting = std::get_if<WaitingForChromeRefresh>(&state_)) {
      snapshot.phase = Phase::WaitingForChromeRefresh;
      FillCacheDiagnostics(waiting->cache, &snapshot);
      snapshot.settlingPreview = waiting->preview;
      snapshot.waitingForChromeRefresh = true;
      snapshot.chromeRefreshTargetVersion = waiting->targetVersion;
      return snapshot;
    }

    return snapshot;
  }

  /// Returns true when cached composited textures are available for presentation.
  [[nodiscard]] bool hasCachedTextures() const { return currentCache().has_value(); }

  /// Returns true when cached composited textures are available for @p entity.
  [[nodiscard]] bool hasCachedTexturesForEntity(Entity entity) const {
    const std::optional<CachedTextures> cache = currentCache();
    return cache.has_value() && cache->entity == entity;
  }

  /// Returns true while a released drag waits for a fresh full render.
  [[nodiscard]] bool isWaitingForFullRender() const {
    return std::holds_alternative<SettlingForRender>(state_);
  }

  /// Returns true while a composited settle waits for refreshed overlay chrome.
  [[nodiscard]] bool isWaitingForChromeRefresh() const {
    return std::holds_alternative<WaitingForChromeRefresh>(state_);
  }

  /// Returns the active preview that should drive presenter-side tile transforms.
  ///
  /// During a live drag this is the tool's active preview. After mouse-up,
  /// \ref SelectTool::activeDragPreview is empty, but the render pane must keep presenting the
  /// released transform until a settled render replaces the cached tiles.
  [[nodiscard]] std::optional<SelectTool::ActiveDragPreview> activePreviewForPresentation(
      const std::optional<SelectTool::ActiveDragPreview>& activePreview) const {
    if (activePreview.has_value()) {
      return activePreview;
    }

    const auto* settling = std::get_if<SettlingForRender>(&state_);
    if (settling != nullptr && settling->cache.has_value() &&
        settling->cache->entity == settling->preview.entity) {
      return settling->preview;
    }

    return std::nullopt;
  }

  /// Returns true when an active drag needs a fresh composited capture.
  [[nodiscard]] bool needsCompositedLayerCapture(
      const std::optional<SelectTool::ActiveDragPreview>& activePreview,
      std::uint64_t /*currentVersion*/, const Vector2i& /*currentCanvasSize*/) const {
    if (!activePreview.has_value()) {
      return false;
    }

    const std::optional<CachedTextures> cache = currentCache();
    // Drag transform writes bump the document version every mouse move. Pure
    // translation stays crisp through presenter-side texture placement, but
    // affine resize/rotate previews can blur a cached bitmap. Refresh those
    // opportunistically only when the cached bitmap represents an older affine
    // transform. Zoom-driven canvas-size changes still settle after the drag so
    // we do not block pointer frames on a full cached-span reraster.
    if (!cache.has_value() || cache->entity != activePreview->entity) {
      return true;
    }

    const SelectTool::ActiveDragPreview representedPreview =
        representedPreviewForActiveCache(*cache, *activePreview);
    if (activePreview->documentFromCachedDocument.isTranslation()) {
      // A pure translation tracks via the cached bitmap's translation offset with
      // no re-capture — UNLESS the cached bitmap is still an affine (we just
      // returned from a resize/rotate to a translation), in which case re-capture
      // a clean, crisply-translated layer instead of carrying the stale affine.
      return !representedPreview.documentFromCachedDocument.isTranslation();
    }

    // Affine (rotate/scale) drag: re-capture a crisp bitmap when the cached
    // bitmap's SCALE has drifted past a threshold (it would otherwise look
    // blurry). The re-capture is intentional (anti-blur); the presentation
    // compensates for the swapped-in image via `represented` (the transform the
    // re-captured bitmap was baked at) so the shape stays continuous across the
    // swap — `effective = represented^-1 * active` re-bases tracking onto the
    // fresh bitmap with no pop. Pure rotation is area-preserving so it never
    // trips this (a rotated bitmap keeps resolution); scaling down downsamples
    // and stays sharp; only upscaling past the threshold re-captures, and a
    // continuous scale-up re-captures in ~threshold steps.
    constexpr double kAffineRecaptureScaleThreshold = 1.5;
    const double activeScale =
        std::sqrt(std::abs(activePreview->documentFromCachedDocument.determinant()));
    const double representedScale =
        std::sqrt(std::abs(representedPreview.documentFromCachedDocument.determinant()));
    if (representedScale < 1e-9) {
      return true;
    }
    const double scaleDrift = activeScale / representedScale;
    return scaleDrift > kAffineRecaptureScaleThreshold ||
           scaleDrift < 1.0 / kAffineRecaptureScaleThreshold;
  }

  /// Returns true when a released drag should request a settled composited refresh.
  [[nodiscard]] bool needsSettledSelectionRefresh(Entity selectedEntity,
                                                  std::uint64_t currentVersion) const {
    const auto* settling = std::get_if<SettlingForRender>(&state_);
    return selectedEntity != entt::null && settling != nullptr &&
           settling->preview.entity == selectedEntity && currentVersion >= settling->targetVersion;
  }

  /// Returns true when the settled refresh must bake the released layer transform.
  [[nodiscard]] bool needsSettledLayerRasterization(Entity selectedEntity,
                                                    std::uint64_t currentVersion) const {
    const auto* settling = std::get_if<SettlingForRender>(&state_);
    return selectedEntity != entt::null && settling != nullptr &&
           settling->preview.entity == selectedEntity &&
           currentVersion >= settling->targetVersion &&
           !settling->preview.documentFromCachedDocument.isTranslation();
  }

  /// Returns true when selection should trigger an async prewarm capture.
  [[nodiscard]] bool shouldPrewarm(Entity selectedEntity,
                                   const std::vector<Entity>& selectedExtraEntities,
                                   std::uint64_t currentVersion, const Vector2i& currentCanvasSize,
                                   bool dragActive) const {
    if (selectedEntity == entt::null || dragActive || isWaitingForFullRender() ||
        isWaitingForChromeRefresh()) {
      return false;
    }

    const std::optional<CachedTextures> cache = currentCache();
    if (!cache.has_value() || cache->entity != selectedEntity || cache->version != currentVersion ||
        cache->canvasSize != currentCanvasSize) {
      return true;
    }

    return representedPreviewForCache(*cache).extraEntities != selectedExtraEntities;
  }

  /// Mark cached composited textures as available for the given entity/version/canvas size.
  void noteCachedTextures(
      Entity entity, std::uint64_t version, const Vector2i& canvasSize,
      std::optional<SelectTool::ActiveDragPreview> representedPreview = std::nullopt) {
    const CachedTextures cache{
        .entity = entity,
        .version = version,
        .canvasSize = canvasSize,
        .representedPreview = std::move(representedPreview),
    };

    if (const auto* settling = std::get_if<SettlingForRender>(&state_)) {
      if (settling->preview.entity == entity) {
        if (version >= settling->targetVersion) {
          state_ = WaitingForChromeRefresh{
              .cache = cache,
              .preview = settling->preview,
              .targetVersion = version,
          };
        } else {
          state_ = SettlingForRender{
              .cache = cache,
              .preview = settling->preview,
              .targetVersion = settling->targetVersion,
          };
        }
        return;
      }
    } else if (const auto* waiting = std::get_if<WaitingForChromeRefresh>(&state_)) {
      if (waiting->preview.entity == entity) {
        state_ = WaitingForChromeRefresh{
            .cache = cache,
            .preview = waiting->preview,
            .targetVersion = waiting->targetVersion,
        };
        return;
      }
    }

    state_ = Cached{.cache = cache};
  }

  /// Drop cached presentation state for @p entity.
  ///
  /// This is intentionally explicit instead of tied to selection changes. Normal deselection keeps
  /// the cached document image alive until the next render replaces it; a selected element becoming
  /// `display:none` is different because the cached promoted tile would keep rendering content that
  /// the live DOM has already hidden.
  ///
  /// @param entity Entity whose cached presentation should be discarded.
  /// @return true if cached state matched @p entity and was cleared.
  bool discardCachedTexturesForEntity(Entity entity) {
    if (entity == entt::null) {
      return false;
    }

    const std::optional<CachedTextures> cache = currentCache();
    if (!cache.has_value() || cache->entity != entity) {
      return false;
    }

    state_ = NoCache{};
    return true;
  }

  /// Finish the settle handoff once overlay chrome and cached AABBs have refreshed to match the
  /// settled document version. Only then is it safe to drop the old drag offset.
  void noteChromeRefreshCompleted(std::uint64_t refreshedVersion) {
    const auto* waiting = std::get_if<WaitingForChromeRefresh>(&state_);
    if (waiting == nullptr || refreshedVersion < waiting->targetVersion) {
      return;
    }

    state_ = Cached{.cache = waiting->cache};
  }

  /// Begin the post-release settling phase, keeping the last composited presentation alive.
  void beginSettling(const std::optional<SelectTool::ActiveDragPreview>& preview,
                     std::uint64_t targetVersion) {
    const std::optional<CachedTextures> cache = currentCache();
    if (!preview.has_value()) {
      state_ = cache.has_value() ? State(Cached{.cache = *cache}) : State(NoCache{});
      return;
    }

    state_ = SettlingForRender{
        .cache = cache,
        .preview = *preview,
        .targetVersion = targetVersion,
    };
  }

  /// End the settling phase once a fresh full render has landed.
  ///
  /// This function only handles settling-state bookkeeping; cached tiles remain live until a fresh
  /// upload replaces them or the document is reset.
  void noteFullRenderLanded(std::uint64_t landedVersion) {
    if (const auto* settling = std::get_if<SettlingForRender>(&state_)) {
      if (landedVersion < settling->targetVersion) {
        return;
      }

      state_ =
          settling->cache.has_value() ? State(Cached{.cache = *settling->cache}) : State(NoCache{});
      return;
    }

    if (const auto* waiting = std::get_if<WaitingForChromeRefresh>(&state_)) {
      state_ = Cached{.cache = waiting->cache};
    }
  }

  /// Returns the drag preview that should currently be displayed, if any.
  [[nodiscard]] std::optional<SelectTool::ActiveDragPreview> presentationPreview(
      const std::optional<SelectTool::ActiveDragPreview>& activePreview) const {
    const std::optional<CachedTextures> cache = currentCache();

    // Prefer an active preview whose entity matches our cached textures. If
    // the active drag has moved to a different entity while a new render is
    // in flight, keep displaying the last cached document image without
    // applying the new entity's live drag offset to stale drag-target tiles.
    if (activePreview.has_value() && cache.has_value() && activePreview->entity == cache->entity) {
      return representedPreviewForActiveCache(*cache, *activePreview);
    }

    if (const auto* settling = std::get_if<SettlingForRender>(&state_);
        settling != nullptr && cache.has_value() && settling->preview.entity == cache->entity) {
      return settling->preview;
    }
    if (const auto* waiting = std::get_if<WaitingForChromeRefresh>(&state_);
        waiting != nullptr && cache.has_value() && waiting->preview.entity == cache->entity) {
      return waiting->preview;
    }
    if (cache.has_value() && cache->entity != entt::null) {
      return SelectTool::ActiveDragPreview{
          .entity = cache->entity,
          .translation = Vector2d::Zero(),
          .documentFromCachedDocument = Transform2d(),
      };
    }
    return std::nullopt;
  }

  /// Preserve in-flight settling across temporary selection remaps.
  ///
  /// After ReplaceDocument, entity handles are invalidated and the selection is remapped to new
  /// entities. Cached composited textures stay live as the visible document image until the next
  /// render atomically replaces them via noteCachedTextures().
  void clearSettlingIfSelectionChanged(Entity selectedEntity, bool dragActive) {
    if (isWaitingForFullRender() || isWaitingForChromeRefresh()) {
      return;
    }

    (void)selectedEntity;
    (void)dragActive;
  }

private:
  [[nodiscard]] std::optional<CachedTextures> currentCache() const {
    if (const auto* cached = std::get_if<Cached>(&state_)) {
      return cached->cache;
    }
    if (const auto* settling = std::get_if<SettlingForRender>(&state_)) {
      return settling->cache;
    }
    if (const auto* waiting = std::get_if<WaitingForChromeRefresh>(&state_)) {
      return waiting->cache;
    }
    return std::nullopt;
  }

  static void FillCacheDiagnostics(const CachedTextures& cache, DiagnosticsSnapshot* snapshot) {
    snapshot->hasCachedTextures = true;
    snapshot->cachedEntity = cache.entity;
    snapshot->cachedVersion = cache.version;
    snapshot->cachedCanvasSize = cache.canvasSize;
  }

  static SelectTool::ActiveDragPreview representedPreviewForCache(const CachedTextures& cache) {
    if (cache.representedPreview.has_value() && cache.representedPreview->entity == cache.entity) {
      return *cache.representedPreview;
    }

    return SelectTool::ActiveDragPreview{
        .entity = cache.entity,
        .translation = Vector2d::Zero(),
        .documentFromCachedDocument = Transform2d(),
    };
  }

  static SelectTool::ActiveDragPreview representedPreviewForActiveCache(
      const CachedTextures& cache, const SelectTool::ActiveDragPreview& activePreview) {
    if (cache.representedPreview.has_value() && cache.representedPreview->entity == cache.entity &&
        cache.representedPreview->dragGeneration == activePreview.dragGeneration) {
      return *cache.representedPreview;
    }

    return SelectTool::ActiveDragPreview{
        .entity = cache.entity,
        .extraEntities = activePreview.extraEntities,
        .translation = Vector2d::Zero(),
        .documentFromCachedDocument = Transform2d(),
        .dragGeneration = activePreview.dragGeneration,
    };
  }

  static bool SameVector(const Vector2d& lhs, const Vector2d& rhs) {
    constexpr double kTolerance = 1e-6;
    return NearEquals(lhs.x, rhs.x, kTolerance) && NearEquals(lhs.y, rhs.y, kTolerance);
  }

  static bool SameTransform(const Transform2d& lhs, const Transform2d& rhs) {
    constexpr double kTolerance = 1e-6;
    for (int i = 0; i < 6; ++i) {
      if (!NearEquals(lhs.data[i], rhs.data[i], kTolerance)) {
        return false;
      }
    }
    return true;
  }

  static bool SameDragPreviewTransform(const SelectTool::ActiveDragPreview& lhs,
                                       const SelectTool::ActiveDragPreview& rhs) {
    return lhs.entity == rhs.entity && lhs.dragGeneration == rhs.dragGeneration &&
           lhs.extraEntities == rhs.extraEntities && SameVector(lhs.translation, rhs.translation) &&
           SameTransform(lhs.documentFromCachedDocument, rhs.documentFromCachedDocument);
  }

  State state_ = NoCache{};
};

/// Print a composited presentation phase for test diagnostics.
inline std::ostream& operator<<(std::ostream& os, CompositedPresentation::Phase phase) {
  switch (phase) {
    case CompositedPresentation::Phase::NoCache: return os << "NoCache";
    case CompositedPresentation::Phase::Cached: return os << "Cached";
    case CompositedPresentation::Phase::SettlingForRender: return os << "SettlingForRender";
    case CompositedPresentation::Phase::WaitingForChromeRefresh:
      return os << "WaitingForChromeRefresh";
  }
  return os << "Unknown";
}

}  // namespace donner::editor
