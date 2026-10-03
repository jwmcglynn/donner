#pragma once
/// @file
/// Owns accepted presentation resources and the intent retained across drag release.

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <variant>
#include <vector>

#include "donner/base/MathUtils.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/SelectTool.h"

namespace donner::editor {

class CompositedPresentation {
public:
  /// Closed presentation phases. A ready resource set already includes its chrome geometry.
  enum class Phase { NoCache, Cached, SettlingForRender };

  /// Detached metadata for scheduler, replay and inspector diagnostics.
  struct DiagnosticsSnapshot {
    Phase phase = Phase::NoCache;
    bool hasCachedTextures = false;
    Entity cachedEntity = entt::null;
    std::uint64_t cachedVersion = 0;
    Vector2i cachedCanvasSize = Vector2i::Zero();
    std::optional<SelectTool::ActiveDragPreview> settlingPreview;
    bool waitingForFullRender = false;
    std::uint64_t settlingTargetVersion = 0;
  };

private:
  struct CachedTextures {
    Entity entity = entt::null;
    std::uint64_t version = 0;
    Vector2i canvasSize = Vector2i::Zero();
    std::optional<SelectTool::ActiveDragPreview> representedPreview;
    std::shared_ptr<const CapturedPresentation> capture;
    std::shared_ptr<const GlTextureCache::PresentationResources> resources;
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
  using State = std::variant<NoCache, Cached, SettlingForRender>;

public:
  /// Inspect the accepted state without borrowing mutable state.
  [[nodiscard]] DiagnosticsSnapshot diagnostics() const {
    DiagnosticsSnapshot result;
    const auto cache = currentCache();
    if (cache.has_value()) {
      result.hasCachedTextures = true;
      result.cachedEntity = cache->entity;
      result.cachedVersion = cache->version;
      result.cachedCanvasSize = cache->canvasSize;
      result.phase = Phase::Cached;
    }
    if (const auto* settling = std::get_if<SettlingForRender>(&state_)) {
      result.phase = Phase::SettlingForRender;
      result.waitingForFullRender = true;
      result.settlingPreview = settling->preview;
      result.settlingTargetVersion = settling->targetVersion;
    }
    return result;
  }

  /// Complete paired resources accepted for drawing.
  [[nodiscard]] std::shared_ptr<const GlTextureCache::PresentationResources> resources() const {
    const auto cache = currentCache();
    return cache.has_value() ? cache->resources : nullptr;
  }

  /// Captured geometry used by non-window bitmap inspection as well as the UI.
  [[nodiscard]] std::shared_ptr<const CapturedPresentation> capturedPresentation() const {
    const auto cache = currentCache();
    return cache.has_value() ? cache->capture : nullptr;
  }

  [[nodiscard]] bool hasCachedTextures() const { return currentCache().has_value(); }
  [[nodiscard]] bool hasCachedTexturesForEntity(Entity entity) const {
    const auto cache = currentCache();
    return cache.has_value() && cache->entity == entity;
  }
  [[nodiscard]] bool isWaitingForFullRender() const {
    return std::holds_alternative<SettlingForRender>(state_);
  }

  /// Retain the released absolute poses until a matching completed render is adopted.
  [[nodiscard]] std::optional<SelectTool::ActiveDragPreview> activePreviewForPresentation(
      const std::optional<SelectTool::ActiveDragPreview>& activePreview) const {
    if (activePreview.has_value()) {
      return activePreview;
    }
    if (const auto* settling = std::get_if<SettlingForRender>(&state_)) {
      return settling->preview;
    }
    return std::nullopt;
  }

  /// Return captured request metadata for diagnostics; never reinterpret its gesture basis.
  [[nodiscard]] std::optional<SelectTool::ActiveDragPreview> presentationPreview(
      const std::optional<SelectTool::ActiveDragPreview>& /*activePreview*/) const {
    const auto cache = currentCache();
    return cache.has_value() ? cache->representedPreview : std::nullopt;
  }

  /// Whether a raster already represents all absolute poses requested by the pointer.
  [[nodiscard]] bool represents(const SelectTool::ActiveDragPreview& active) const {
    const auto cache = currentCache();
    if (!cache.has_value()) {
      return false;
    }
    if (cache->capture != nullptr) {
      const auto requestedDocumentFromCapturedDocument =
          ResolvePresentationTransform(cache->capture->poses(), active.poses);
      return cache->capture->identity().sameContent(active.contentIdentity) &&
             requestedDocumentFromCapturedDocument.has_value() &&
             SamePresentationTransform(*requestedDocumentFromCapturedDocument, Transform2d());
    }
    return cache->representedPreview.has_value() &&
           cache->representedPreview->entity == active.entity &&
           cache->representedPreview->dragGeneration == active.dragGeneration &&
           cache->representedPreview->translation == active.translation &&
           SamePresentationTransform(cache->representedPreview->documentFromCachedDocument,
                                     active.documentFromCachedDocument);
  }

  /// Determine whether a new raster is required for coverage or affine sampling quality.
  [[nodiscard]] bool needsCompositedLayerCapture(
      const std::optional<SelectTool::ActiveDragPreview>& active, std::uint64_t /*version*/,
      const Vector2i& /*canvasSize*/,
      double translationRecaptureDistanceDoc = std::numeric_limits<double>::infinity()) const {
    if (!active.has_value()) {
      return false;
    }
    const auto cache = currentCache();
    if (!cache.has_value() || cache->entity != active->entity) {
      return true;
    }
    const auto requestedDocumentFromCapturedDocument =
        resolveRequestedDocumentFromCapturedDocument(*cache, *active);
    if (!requestedDocumentFromCapturedDocument.has_value()) {
      return true;
    }
    if (std::abs(requestedDocumentFromCapturedDocument->determinant()) < 1e-12) {
      return false;
    }
    if (requestedDocumentFromCapturedDocument->isTranslation()) {
      const auto translation = requestedDocumentFromCapturedDocument->translation();
      return std::isfinite(translationRecaptureDistanceDoc) &&
             translationRecaptureDistanceDoc > 0.0 &&
             std::max(std::abs(translation.x), std::abs(translation.y)) >
                 translationRecaptureDistanceDoc;
    }
    const double scaleX = std::hypot(requestedDocumentFromCapturedDocument->data[0],
                                     requestedDocumentFromCapturedDocument->data[1]);
    const double scaleY = std::hypot(requestedDocumentFromCapturedDocument->data[2],
                                     requestedDocumentFromCapturedDocument->data[3]);
    return std::max(scaleX, scaleY) > 1.5 || std::min(scaleX, scaleY) < 1.0 / 1.5;
  }

  [[nodiscard]] bool needsSettledSelectionRefresh(Entity entity, std::uint64_t version) const {
    const auto* settling = std::get_if<SettlingForRender>(&state_);
    return entity != entt::null && settling != nullptr && settling->preview.entity == entity &&
           version >= settling->targetVersion;
  }
  [[nodiscard]] bool needsSettledLayerRasterization(Entity entity, std::uint64_t version) const {
    const auto* settling = std::get_if<SettlingForRender>(&state_);
    return needsSettledSelectionRefresh(entity, version) && settling != nullptr &&
           !settling->preview.documentFromCachedDocument.isTranslation();
  }

  [[nodiscard]] bool shouldPrewarm(Entity entity, const std::vector<Entity>& extraEntities,
                                   std::uint64_t version, const Vector2i& canvasSize,
                                   bool dragActive) const {
    if (entity == entt::null || dragActive || isWaitingForFullRender()) {
      return false;
    }
    const auto cache = currentCache();
    if (!cache.has_value() || cache->entity != entity || cache->version != version ||
        cache->canvasSize != canvasSize) {
      return true;
    }
    return cache->representedPreview.has_value()
               ? cache->representedPreview->extraEntities != extraEntities
               : !extraEntities.empty();
  }

  /// Adopt a fully prepared resource/geometry pair in one transition.
  void notePreparedResources(std::shared_ptr<const GlTextureCache::PresentationResources> resources,
                             Entity entity,
                             std::optional<SelectTool::ActiveDragPreview> representedPreview,
                             bool currentCommittedScene = false) {
    if (resources == nullptr || resources->capture() == nullptr) {
      return;
    }
    const auto capture = resources->capture();
    accept(CachedTextures{.entity = entity,
                          .version = capture->identity().version,
                          .canvasSize = resources->coverage().activeOutputSizePx,
                          .representedPreview = std::move(representedPreview),
                          .capture = capture,
                          .resources = std::move(resources)},
           currentCommittedScene);
  }

  /// Record a complete non-window raster cache, which does not own GPU registrations.
  void noteCachedTextures(
      Entity entity, std::uint64_t version, const Vector2i& canvasSize,
      std::optional<SelectTool::ActiveDragPreview> representedPreview = std::nullopt,
      std::shared_ptr<const CapturedPresentation> capture = nullptr) {
    accept(CachedTextures{.entity = entity,
                          .version = version,
                          .canvasSize = canvasSize,
                          .representedPreview = std::move(representedPreview),
                          .capture = std::move(capture)});
  }

  /// Invalidate the selected-layer cache key while retaining any complete scene for replacement.
  /// @param entity Selected-layer owner being invalidated.
  bool discardCachedTexturesForEntity(Entity entity) {
    const auto cache = currentCache();
    if (entity == entt::null || !cache.has_value() || cache->entity != entity) {
      return false;
    }
    if (cache->resources != nullptr) {
      auto retained = *cache;
      retained.entity = entt::null;
      retained.representedPreview.reset();
      state_ = Cached{std::move(retained)};
    } else {
      state_ = NoCache{};
    }
    return true;
  }

  void beginSettling(const std::optional<SelectTool::ActiveDragPreview>& preview,
                     std::uint64_t targetVersion) {
    const auto cache = currentCache();
    if (!preview.has_value()) {
      state_ = cache.has_value() ? State(Cached{*cache}) : State(NoCache{});
      return;
    }
    state_ = SettlingForRender{cache, *preview, targetVersion};
  }

  /// Stop retaining a released intent after a different actual selection replaces it.
  void clearSettlingIfSelectionChanged(Entity entity, bool dragActive) {
    const auto* settling = std::get_if<SettlingForRender>(&state_);
    if (settling != nullptr && !dragActive && entity != settling->preview.entity) {
      const auto cache = settling->cache;
      state_ = cache.has_value() ? State(Cached{*cache}) : State(NoCache{});
    }
  }

private:
  static std::optional<Transform2d> resolveRequestedDocumentFromCapturedDocument(
      const CachedTextures& cache, const SelectTool::ActiveDragPreview& active) {
    if (cache.capture != nullptr) {
      if (!cache.capture->identity().sameContent(active.contentIdentity)) {
        return std::nullopt;
      }
      return ResolvePresentationTransform(cache.capture->poses(), active.poses);
    }
    if (!cache.representedPreview.has_value() ||
        cache.representedPreview->dragGeneration != active.dragGeneration ||
        std::abs(cache.representedPreview->documentFromCachedDocument.determinant()) < 1e-12) {
      return std::nullopt;
    }
    return cache.representedPreview->documentFromCachedDocument.inverse() *
           active.documentFromCachedDocument;
  }

  static bool settledCaptureMatches(const CachedTextures& cache,
                                    const SettlingForRender& settling) {
    if (cache.version < settling.targetVersion) {
      return false;
    }
    if (cache.version > settling.targetVersion) {
      return true;
    }
    if (cache.capture == nullptr || settling.preview.poses.empty()) {
      return true;
    }
    if (cache.capture->identity().documentGeneration !=
            settling.preview.contentIdentity.documentGeneration ||
        cache.capture->identity().geometryRevision !=
            settling.preview.contentIdentity.geometryRevision) {
      return true;
    }
    const auto requestedDocumentFromCapturedDocument =
        ResolvePresentationTransform(cache.capture->poses(), settling.preview.poses);
    return requestedDocumentFromCapturedDocument.has_value() &&
           SamePresentationTransform(*requestedDocumentFromCapturedDocument, Transform2d());
  }

  void accept(CachedTextures cache, bool currentCommittedScene = false) {
    if (const auto* settling = std::get_if<SettlingForRender>(&state_);
        settling != nullptr && !currentCommittedScene && !settledCaptureMatches(cache, *settling)) {
      state_ = SettlingForRender{std::move(cache), settling->preview, settling->targetVersion};
    } else {
      state_ = Cached{std::move(cache)};
    }
  }

  [[nodiscard]] std::optional<CachedTextures> currentCache() const {
    if (const auto* cached = std::get_if<Cached>(&state_)) {
      return cached->cache;
    }
    if (const auto* settling = std::get_if<SettlingForRender>(&state_)) {
      return settling->cache;
    }
    return std::nullopt;
  }
  State state_ = NoCache{};
};

inline std::ostream& operator<<(std::ostream& os, CompositedPresentation::Phase phase) {
  switch (phase) {
    case CompositedPresentation::Phase::NoCache: return os << "NoCache";
    case CompositedPresentation::Phase::Cached: return os << "Cached";
    case CompositedPresentation::Phase::SettlingForRender: return os << "SettlingForRender";
  }
  return os << "Unknown";
}

}  // namespace donner::editor
