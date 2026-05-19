#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <ostream>
#include <variant>

#include "donner/base/EcsRegistry.h"
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

  /// Returns true while a released drag waits for a fresh full render.
  [[nodiscard]] bool isWaitingForFullRender() const {
    return std::holds_alternative<SettlingForRender>(state_);
  }

  /// Returns true while a composited settle waits for refreshed overlay chrome.
  [[nodiscard]] bool isWaitingForChromeRefresh() const {
    return std::holds_alternative<WaitingForChromeRefresh>(state_);
  }

  /// Returns true when an active drag needs a fresh composited capture.
  [[nodiscard]] bool needsCompositedLayerCapture(
      const std::optional<SelectTool::ActiveDragPreview>& activePreview,
      std::uint64_t currentVersion, const Vector2i& currentCanvasSize) const {
    if (!activePreview.has_value()) {
      return false;
    }

    const std::optional<CachedTextures> cache = currentCache();
    return !cache.has_value() || cache->entity != activePreview->entity ||
           cache->version != currentVersion || cache->canvasSize != currentCanvasSize;
  }

  /// Returns true when a released drag should request a settled composited refresh.
  [[nodiscard]] bool needsSettledSelectionRefresh(Entity selectedEntity,
                                                  std::uint64_t currentVersion) const {
    const auto* settling = std::get_if<SettlingForRender>(&state_);
    return selectedEntity != entt::null && settling != nullptr &&
           settling->preview.entity == selectedEntity && currentVersion >= settling->targetVersion;
  }

  /// Returns true when selection should trigger an async prewarm capture.
  [[nodiscard]] bool shouldPrewarm(Entity selectedEntity, std::uint64_t currentVersion,
                                   const Vector2i& currentCanvasSize, bool dragActive) const {
    if (selectedEntity == entt::null || dragActive || isWaitingForFullRender() ||
        isWaitingForChromeRefresh()) {
      return false;
    }

    const std::optional<CachedTextures> cache = currentCache();
    return !cache.has_value() || cache->entity != selectedEntity ||
           cache->version != currentVersion || cache->canvasSize != currentCanvasSize;
  }

  /// Mark cached composited textures as available for the given entity/version/canvas size.
  void noteCachedTextures(Entity entity, std::uint64_t version, const Vector2i& canvasSize) {
    const CachedTextures cache{
        .entity = entity,
        .version = version,
        .canvasSize = canvasSize,
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
      return activePreview;
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
