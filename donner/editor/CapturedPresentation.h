#pragma once
/// @file
/// Selection geometry and object poses captured under the raster's document access guard.

#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/PresentationPose.h"
#include "donner/svg/SVGDocument.h"

namespace donner::editor {

/// Immutable geometry/provenance paired with one render result.
class CapturedPresentation {
public:
  /// Text layout annotations captured under the same mutation revision as their glyphs.
  struct TextEditing {
    PresentationIdentity identity;
    Vector2i canvasSize = Vector2i::Zero();
    Entity subject = entt::null;
    std::optional<SelectionChromeSnapshot::TextCaret> caret;
    std::optional<std::array<Vector2d, 4>> frame;
    float frameOpacity = 1.0f;
    std::vector<std::array<Vector2d, 4>> selection;
  };
  /// Text annotations owned by this capture and measured against its actual document revision.
  [[nodiscard]] const std::optional<TextEditing>& textEditing() const UTILS_LIFETIME_BOUND {
    return textEditing_;
  }
  /**
   * Capture geometry while the caller owns the document state used to render the pixels.
   * @param document Document protected by the render's write access.
   * @param identity Provenance of that render.
   * @param selection Selected objects in document order.
   * @param tracked Objects whose previously displayed poses must survive render adoption.
   * @param independentlyMovable Renderer-certified independently owned paint layers.
   * @param sourceHover Source highlights to anchor to this scene.
   * @param lockedFlash Rejected-selection feedback to anchor to this scene.
   * @param livePath Optional solid vector replacement supplying both paint and geometry.
   * @param textEditing Text annotations accepted only at the capture's actual mutation revision.
   * @return Immutable paired geometry, or null when a selected handle belongs to another document.
   */
  [[nodiscard]] static std::shared_ptr<const CapturedPresentation> Capture(
      svg::SVGDocument& document, PresentationIdentity identity,
      std::span<const svg::SVGElement> selection, std::span<const Entity> tracked = {},
      std::span<const Entity> independentlyMovable = {},
      std::span<const svg::SVGElement> sourceHover = {},
      const std::optional<LockedRejectionFlashInput>& lockedFlash = std::nullopt,
      const std::optional<svg::SVGElement>& livePath = std::nullopt,
      const std::optional<TextEditing>& textEditing = std::nullopt);

  /// Capture provenance owned by this immutable snapshot.
  [[nodiscard]] const PresentationIdentity& identity() const UTILS_LIFETIME_BOUND {
    return identity_;
  }
  /// Selected object identities owned by this snapshot.
  [[nodiscard]] const std::vector<Entity>& selection() const UTILS_LIFETIME_BOUND {
    return selection_;
  }
  /// Absolute object poses sampled with the geometry.
  [[nodiscard]] const std::vector<PresentationPose>& poses() const UTILS_LIFETIME_BOUND {
    return poses_;
  }
  /// Selection geometry attributed to one object, rather than a gesture-wide coordinate guess.
  struct Object {
    Entity entity = entt::null;
    SelectionChromeSnapshot chrome;
    bool pathBoundsCoverFrame = false;
    std::vector<Box2d> paintBoundsDoc;
  };
  /// Additional annotation geometry anchored to a captured object or its independent ancestor.
  struct Adornment {
    Entity subject = entt::null;
    Entity owner = entt::null;
    bool lockedFlash = false;
    SelectionChromeSnapshot chrome;
  };
  [[nodiscard]] const std::vector<Adornment>& adornments() const UTILS_LIFETIME_BOUND {
    return adornments_;
  }
  /// Objects and their uncropped geometry, owned for the lifetime of this capture.
  [[nodiscard]] const std::vector<Object>& objects() const UTILS_LIFETIME_BOUND { return objects_; }
  /// Actual pose consumed by the raster, including tracked objects outside the current selection.
  [[nodiscard]] std::optional<PresentationPose> pose(Entity entity) const;
  /// Conservative paint regions for a tracked object, including objects outside the selection.
  [[nodiscard]] std::vector<Box2d> paintBounds(Entity entity) const {
    const auto found = trackedPaintBounds_.find(entity);
    return found == trackedPaintBounds_.end() ? std::vector<Box2d>() : found->second;
  }
  /// Whether a previously tracked object was absent from this captured document.
  [[nodiscard]] bool absent(Entity entity) const { return absent_.contains(entity); }
  /// Whether one object's pixels are independent of a fixed ancestor compositing context.
  [[nodiscard]] bool canProject(Entity entity) const {
    return independentlyMovable_.contains(entity) && !rasterBound_.contains(entity);
  }
  /// Renderer-certified independent objects, preserved by same-scene selection recapture.
  [[nodiscard]] std::vector<Entity> independentlyMovable() const {
    return {independentlyMovable_.begin(), independentlyMovable_.end()};
  }
  /// Canvas dimensions used when the layout and geometry were captured.
  [[nodiscard]] Vector2i canvasSize() const { return canvasSize_; }
  /// Author-space origin removed by the raster tile producer.
  [[nodiscard]] Vector2d documentOrigin() const { return documentOrigin_; }
  /// False when selected pixels depend on an ancestor's fixed compositing context.
  [[nodiscard]] bool affinePreviewAllowed() const { return affinePreviewAllowed_; }

private:
  CapturedPresentation() = default;
  bool captureSelected(svg::SVGDocument& document, std::span<const svg::SVGElement> selection,
                       const std::optional<svg::SVGElement>& livePath);
  void captureTracked(svg::SVGDocument& document, std::span<const Entity> tracked);
  void captureValidatedAdornments(svg::SVGDocument& document,
                                  std::span<const svg::SVGElement> sourceHover,
                                  const std::optional<LockedRejectionFlashInput>& lockedFlash);
  PresentationIdentity identity_;
  std::optional<TextEditing> textEditing_;
  std::vector<Entity> selection_;
  std::vector<PresentationPose> poses_;
  static std::optional<svg::SVGElement> findAttachedElement(svg::SVGDocument& document,
                                                            Entity entity);
  void capturePose(const svg::SVGElement& element);
  Entity adornmentOwner(const svg::SVGElement& element) const;
  void captureAdornments(std::span<const svg::SVGElement> sourceHover,
                         const std::optional<LockedRejectionFlashInput>& lockedFlash);
  std::vector<Object> objects_;
  std::vector<Adornment> adornments_;
  std::unordered_map<Entity, PresentationPose> trackedPoses_;
  std::unordered_map<Entity, std::vector<Box2d>> trackedPaintBounds_;
  std::unordered_set<Entity> absent_;
  std::unordered_set<Entity> rasterBound_;
  std::unordered_set<Entity> independentlyMovable_;
  Vector2i canvasSize_;
  Vector2d documentOrigin_;
  bool affinePreviewAllowed_ = true;
};

}  // namespace donner::editor
