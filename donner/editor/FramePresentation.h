#pragma once
/// @file
/// One immutable geometry/resource decision shared by document and selection drawing.

#include <memory>
#include <optional>
#include <vector>

#include "donner/editor/CapturedPresentation.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/ViewportState.h"

namespace donner::editor {

/// Resolve one camera mapping shared by pixel and chrome placement.
/// @param viewport Camera and logical pane geometry.
/// @param framebufferFromLogicalScale Physical pixels per logical pixel, independently per axis.
Transform2d PresentedFramebufferFromDocumentTransform(const ViewportState& viewport,
                                                      const Vector2d& framebufferFromLogicalScale);

/// Intersect the artboard with the pane, refusing invalid or empty rectangles.
/// @param paneRect Logical pane clip.
/// @param imageRect Artboard extent in the same coordinate space.
std::optional<Box2d> PresentedImageClipRect(const Box2d& paneRect, const Box2d& imageRect);

/// Transient UI adornments, distinct from selection geometry captured with the document pixels.
struct PresentationDecorations {
  std::optional<Box2d> marqueeDoc;
  std::vector<Entity> sourceHover;
  Entity lockedFlashEntity = entt::null;
  float lockedFlashIntensity = 0.0f;
  std::optional<Path> penPreviewSegmentDoc;
  std::optional<Vector2d> penCloseAffordanceDoc;
  std::optional<CapturedPresentation::TextEditing> textEditing;
  std::optional<SelectionChromeSnapshot::TextBoxDragPreview> textBoxDragPreviewDoc;
};

class FramePresentation;

/// The missing proof that prevented an otherwise coherent frame from being installed.
enum class FramePresentationFailure {
  None,
  MissingResources,
  InvalidInput,
  MissingOverview,
  IncompatiblePose,
  InsufficientCoverage,
  MissingSelectionGeometry,
  UploadRefused
};

/// A sealed frame, when available, and the exact work needed to complete its presentation.
struct FramePresentationBuildResult {
  std::shared_ptr<const FramePresentation> frame;
  FramePresentationFailure failure = FramePresentationFailure::None;
};

/// Input intent consumed once while sealing a frame; draw callbacks cannot consult it later.
struct FramePresentationInput {
  std::uint64_t frameId = 0;
  ViewportState viewport;
  Box2d paneClipRect;
  std::vector<Entity> selection;
  PresentationIdentity documentIdentity;
  bool pendingDocumentMutations = false;  //!< Input not yet incorporated into a guarded capture.
  std::optional<SelectTool::ActiveDragPreview> desired;
  SelectionChromeDetail detail = SelectionChromeDetail::Full;
  PresentationDecorations decorations;
  Entity suppressedLayerEntity = entt::null;
  bool suppressSelectionPixels = false;
  bool includeChrome = true;
  /// Complete vector paint and geometry replacing one independently owned raster layer.
  std::shared_ptr<const CapturedPresentation> livePathReplacement;
};

/// Immutable, validated pixel/geometry binding used by every draw pass for a presented frame.
class FramePresentation {
public:
  /**
   * Resolve presentation from a complete resource manifest, without reading a live document.
   * @param resources Atomically published textures and matching captured scene.
   * @param input This UI frame's pointer, selection and camera intent.
   * @param selectionCapture Optional new selection captured from exactly the same scene revision.
   * @param previous Prior frame, used to retain text adornments while newer text is rendering.
   * @return A sealed frame, or null when the resource/geometry binding is invalid.
   */
  [[nodiscard]] static FramePresentationBuildResult Build(
      std::shared_ptr<const GlTextureCache::PresentationResources> resources,
      const FramePresentationInput& input,
      std::shared_ptr<const CapturedPresentation> selectionCapture = nullptr,
      std::shared_ptr<const FramePresentation> previous = nullptr);

  /// Select the exact raster source against which selection geometry may be recaptured.
  /// @param resources Complete active/overview resource manifest.
  /// @param input Camera intent used by Build for the same frame.
  /// @return Selected immutable capture, or null if no complete source covers this camera.
  [[nodiscard]] static std::shared_ptr<const CapturedPresentation> CaptureForFrame(
      const std::shared_ptr<const GlTextureCache::PresentationResources>& resources,
      const FramePresentationInput& input);

  /// Check whether a candidate can preserve every affine pose already visible to the user.
  /// @param capture Candidate geometry and pose provenance.
  /// @param tiles Candidate renderer-owned tile identities.
  /// @param previous Last installed frame, or null before first presentation.
  [[nodiscard]] static bool CanAdopt(const CapturedPresentation& capture,
                                     std::span<const RenderResult::CompositedTile> tiles,
                                     const FramePresentation* previous,
                                     bool latestCommittedScene = false);
  /// Objects still shown at a pose that has not been incorporated into their raster capture.
  [[nodiscard]] const std::vector<PresentationPose>& overrides() const UTILS_LIFETIME_BOUND {
    return overrides_;
  }
  /// Solid vector paint replacing the raster layer at its original paint-order position.
  [[nodiscard]] const std::optional<SelectionChromeSnapshot>& replacementPaint() const
      UTILS_LIFETIME_BOUND {
    return replacementPaint_;
  }
  /// Frame identity shared by both GPU passes and any diagnostic capture.
  [[nodiscard]] std::uint64_t frameId() const { return frameId_; }
  /// Geometry source for selected objects, including a complete live vector replacement.
  [[nodiscard]] PresentationIdentity selectionIdentity() const {
    return livePathCapture_ != nullptr ? livePathCapture_->identity()
                                       : selectionCapture_->identity();
  }
  /// Raster/geometry provenance of this frame.
  [[nodiscard]] const PresentationIdentity& identity() const UTILS_LIFETIME_BOUND {
    return identity_;
  }
  /// One camera used by both document and chrome.
  [[nodiscard]] const ViewportState& viewport() const UTILS_LIFETIME_BOUND { return viewport_; }
  /// Clip region shared by the document and editor chrome.
  [[nodiscard]] const Box2d& paneClipRect() const UTILS_LIFETIME_BOUND { return paneClipRect_; }
  /// Visible artboard portion, captured with the camera.
  [[nodiscard]] const std::optional<Box2d>& documentClipRect() const UTILS_LIFETIME_BOUND {
    return documentClipRect_;
  }
  /// Apply the target framebuffer scale to this frame's one document camera.
  /// @param framebufferFromLogicalScale Physical pixels per logical pixel on the target.
  [[nodiscard]] Transform2d framebufferFromDocument(
      const Vector2d& framebufferFromLogicalScale) const {
    return screenFromDocument_ * Transform2d::Scale(framebufferFromLogicalScale);
  }
  /// Whether this frame includes editor chrome, rather than a content-only capture.
  [[nodiscard]] bool chromeEnabled() const { return chromeEnabled_; }
  /// Active raster geometry already resolved to the chosen absolute poses.
  [[nodiscard]] const std::vector<GlTextureCache::TileView>& tiles() const UTILS_LIFETIME_BOUND {
    return tiles_;
  }
  /// Compatible overview geometry; it never supplies a different selected-object pose.
  [[nodiscard]] const std::vector<GlTextureCache::TileView>& overviewTiles() const
      UTILS_LIFETIME_BOUND {
    return overviewTiles_;
  }
  /// Selection geometry resolved from the same capture and mapping as the tiles.
  [[nodiscard]] const SelectionChromeSnapshot& chrome() const UTILS_LIFETIME_BOUND {
    return chrome_;
  }
  /// Unculled selected bounds for hit testing and handles, including content-only captures.
  [[nodiscard]] const std::vector<Box2d>& selectionBounds() const UTILS_LIFETIME_BOUND {
    return selectionBounds_;
  }
  /// Whether selection geometry belongs to this frame's selected object set.
  [[nodiscard]] bool hasSelectionGeometry() const { return hasSelectionGeometry_; }
  /// Whether the frame applied pointer intent to independently movable captured pixels.
  [[nodiscard]] bool followsPointer() const { return followsPointer_; }
  /// Chosen absolute object poses, also used to derive selection geometry.
  [[nodiscard]] const std::vector<PresentationPose>& poses() const UTILS_LIFETIME_BOUND {
    return poses_;
  }

private:
  FramePresentation() = default;
  void initialize(const FramePresentationInput& input, bool useOverview);
  bool retainDisplayedPoses(const FramePresentation* previous);
  bool chosenPosesHaveCoverage(const FramePresentationInput& input) const;
  void applyPointerIntent(const FramePresentationInput& input);
  void projectSelectionGeometry();
  void resolveSelectionBounds(const FramePresentationInput& input);
  void resolveTiles(const FramePresentationInput& input);
  void finishChrome(const FramePresentationInput& input, const FramePresentation* previous);
  void applyLivePathReplacement(const FramePresentationInput& input);
  bool textMatchesFrame(const CapturedPresentation::TextEditing& text,
                        std::span<const Entity> selection) const;
  void applyDecorations(const FramePresentationInput& input, const FramePresentation* previous);
  void applyDetail(SelectionChromeDetail detail);
  std::optional<SelectionChromeSnapshot> liveAdornment(
      const CapturedPresentation::Adornment& item) const;
  SelectionChromeSnapshot projectAdornment(const CapturedPresentation::Adornment& item) const;
  void applyCapturedAdornments(const FramePresentationInput& input);
  std::shared_ptr<const GlTextureCache::PresentationResources> resources_;
  std::shared_ptr<const CapturedPresentation> selectionCapture_;
  std::shared_ptr<const CapturedPresentation> rasterCapture_;
  std::uint64_t frameId_ = 0;
  PresentationIdentity identity_;
  ViewportState viewport_;
  Box2d paneClipRect_;
  std::optional<Box2d> documentClipRect_;
  Transform2d screenFromDocument_;
  bool chromeEnabled_ = true;
  bool useOverview_ = false;
  std::vector<GlTextureCache::TileView> tiles_;
  std::vector<GlTextureCache::TileView> overviewTiles_;
  std::vector<PresentationPose> poses_;
  std::vector<PresentationPose> overrides_;
  SelectionChromeSnapshot chrome_;
  std::vector<Box2d> selectionBounds_;
  std::optional<SelectionChromeSnapshot> replacementPaint_;
  std::shared_ptr<const CapturedPresentation> livePathCapture_;
  bool hasSelectionGeometry_ = false;
  bool followsPointer_ = false;
};

}  // namespace donner::editor
