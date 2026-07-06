#pragma once
/// @file

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/ViewportState.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

struct TreeViewState {
  std::optional<svg::SVGElement> scrollTarget;
  bool pendingScroll = false;
  bool selectionChangedInTree = false;
};

/// Scale-rotate-translate view of a transform, used by the inspector's
/// editable transform fields. The equivalent matrix is
/// `Scale(scale) * Rotate(rotationRadians) * Translate(translation)` with
/// Donner's apply-left-to-right composition (scale first, translation last).
struct DecomposedTransform {
  Vector2d translation;                 ///< Translation components (e, f).
  double rotationRadians = 0.0;         ///< Rotation angle, in radians.
  Vector2d scale = Vector2d(1.0, 1.0);  ///< Per-axis scale. `scale.y` is negative for flips.
};

/// Decompose @p transform into scale, rotation, and translation.
///
/// Returns `std::nullopt` when the matrix cannot be represented without a
/// skew component (its basis columns are not orthogonal) or is singular
/// (zero-length x basis). Callers should fall back to raw matrix editing in
/// that case rather than force-fitting the fields.
[[nodiscard]] std::optional<DecomposedTransform> DecomposeTransform(const Transform2d& transform);

/// Compose the decomposed fields back into a matrix: scale is applied first,
/// then rotation, then translation. Inverse of \ref DecomposeTransform for
/// every decomposable matrix.
[[nodiscard]] Transform2d ComposeTransform(const DecomposedTransform& decomposed);

/// Renders the editor's tree view and inspector panes.
///
/// The panes are always rendered from an internal snapshot so they stay
/// visible even while the async renderer is mutating the document (the
/// "(rendering...)" placeholder used to cover this gap, which made the panes
/// flash to a disabled message on every render). The snapshot is refreshed
/// from the live `EditorApp` when the caller indicates the worker thread
/// isn't touching the document; otherwise the most recent capture is
/// replayed unchanged. Click handling is gated the same way so mutations
/// can't race the worker.
class SidebarPresenter {
public:
  /// Which inspector transform widget owns the in-progress edit.
  enum class TransformField : std::uint8_t {
    PositionX,  ///< Bounds left edge, document space.
    PositionY,  ///< Bounds top edge, document space.
    Width,      ///< Bounds width, document space.
    Height,     ///< Bounds height, document space.
    Rotation,   ///< Decomposed rotation, degrees.
    Matrix,     ///< One raw matrix component (see `matrixIndex`).
  };

  /// One-shot transform action for the Position region's rotate / flip
  /// buttons (QA-F16). Unlike the drag-driven \ref TransformField edits these
  /// apply a single discrete transform and record one undo entry immediately.
  enum class DiscreteTransform : std::uint8_t {
    Rotate90,        ///< Rotate 90 degrees clockwise about the bounds center.
    FlipHorizontal,  ///< Mirror across the vertical center axis.
    FlipVertical,    ///< Mirror across the horizontal center axis.
  };

  /// Maps a static path-operation icon bitmap to an ImGui texture handle for
  /// display. The icon bitmaps are rendered from embedded Bootstrap SVG resources
  /// through Donner; ImGui only receives the final raster texture for the image
  /// button.
  ///
  /// @param stableId Stable id of the static icon resource being uploaded.
  /// @param bitmap The Donner-rendered RGBA icon bitmap.
  /// @return An ImGui texture handle plus the valid payload UV range, or an
  ///   empty texture if upload failed.
  struct IconTexture {
    ImTextureID texture = 0;                      ///< ImGui texture handle.
    Vector2d uvBottomRight = Vector2d(1.0, 1.0);  ///< Bottom-right valid payload UV.
  };
  using IconTextureProvider =
      std::function<IconTexture(std::uint64_t stableId, const svg::RendererBitmap& bitmap)>;

  /// Refresh the tree / inspector snapshot from live app state. Safe to
  /// call only when the async renderer is idle.
  void refreshSnapshot(const EditorApp& app);

  /// Render the tree pane from the current snapshot. When @p liveApp is
  /// non-null, click-induced selection mutations are applied to it; when
  /// null, clicks are dropped (the render is "read-only" because the worker
  /// thread owns the document).
  ///
  /// @param iconTextureProvider Uploads the shared disclosure-chevron mask to an
  ///   ImGui texture; pass null (e.g. headless tests) to skip chevron art while
  ///   keeping the disclosure interaction.
  void renderTreeView(EditorApp* liveApp, TreeViewState& state,
                      const IconTextureProvider& iconTextureProvider = {}) const;

  /**
   * Render the inspector pane from the current snapshot.
   *
   * @param liveApp Live editor app for button actions, or null while the
   *   renderer owns the document.
   * @param viewport Viewport state diagnostics.
   * @param iconTextureProvider Uploads static Donner-rendered path operation
   *   icon bitmaps to ImGui textures for display, or null to keep blank hit
   *   areas in headless tests.
   * @param renderPaintSections Rendered between the Position region and the
   *   Path Operations section for a single-element selection (the Fill /
   *   Stroke sections, QA-F15). Owned by `EditorShell` because it needs the
   *   shell's paint machinery and color pickers; empty in headless tests.
   *   Returns true if it queued a document mutation.
   * @return true if an inspector action queued a document mutation.
   */
  bool renderInspector(EditorApp* liveApp, const ViewportState& viewport,
                       const IconTextureProvider& iconTextureProvider = {},
                       const std::function<bool()>& renderPaintSections = {});

  [[nodiscard]] bool inspectorHasSelectionForTesting() const {
    return inspectorSnapshot_.hasSelection;
  }

  [[nodiscard]] bool hasTreeSnapshotForTesting() const { return treeSnapshot_.has_value(); }

  /// Whether the tree node for @p entityId is expanded. Keyed by the element's
  /// 32-bit entity id (`entity` cast). Drives the disclosure round-trip test.
  [[nodiscard]] bool isTreeNodeExpandedForTesting(std::uint32_t entityId) const {
    return treeExpandedEntities_.count(entityId) != 0;
  }

  /// Toggle the tree disclosure for @p entityId, exactly as clicking the row's
  /// chevron does.
  void toggleTreeNodeExpandedForTesting(std::uint32_t entityId) {
    toggleTreeNodeExpanded(entityId);
  }

  [[nodiscard]] std::string_view inspectorTitleForTesting() const {
    return inspectorSnapshot_.titleText;
  }

  [[nodiscard]] std::span<const std::pair<std::string, std::string>>
  inspectorXmlAttributesForTesting() const {
    return inspectorSnapshot_.xmlAttributes;
  }

  [[nodiscard]] std::span<const std::pair<std::string, std::string>>
  inspectorComputedStyleForTesting() const {
    return inspectorSnapshot_.computedStyle;
  }

  [[nodiscard]] const std::optional<Box2d>& inspectorBoundsForTesting() const {
    return inspectorSnapshot_.bounds;
  }

  [[nodiscard]] const std::optional<Transform2d>& inspectorTransformForTesting() const {
    return inspectorSnapshot_.transform;
  }

  // Testing hooks that drive the transform-edit state machine directly,
  // mirroring the widget lifecycle (activate -> per-frame value writes ->
  // deactivate) without an interactive ImGui frame.

  /// Capture the edit baseline for @p field, as widget activation does.
  void beginTransformEditForTesting(EditorApp& app, TransformField field, int matrixIndex = 0) {
    beginTransformEdit(app, field, matrixIndex, "Edit transform");
  }

  /// Whether a transform edit is currently in progress.
  [[nodiscard]] bool hasTransformEditForTesting() const { return transformEdit_.has_value(); }

  /// Write @p value into the active edit and queue the resulting mutation,
  /// as one frame of dragging does. Returns true if a mutation was queued.
  bool applyTransformEditForTesting(EditorApp& app, double value) {
    if (!transformEdit_.has_value()) {
      return false;
    }
    if (transformEdit_->field == TransformField::Matrix) {
      transformEdit_->matrixValues[static_cast<std::size_t>(transformEdit_->matrixIndex)] = value;
    }
    transformEdit_->fieldValue = value;
    return applyTransformEdit(app, value);
  }

  /// Finalize the active edit into one undo entry, as widget deactivation does.
  void commitTransformEditForTesting(EditorApp& app) { commitTransformEdit(app); }

private:
  struct TreeNodeSnapshot {
    /// Captured element reference. Valid for as long as the underlying
    /// entity isn't destroyed - for light-tree nodes that only happens
    /// on a full document rebuild (`resetAllLayers` / document reload),
    /// at which point the snapshot is refreshed on the next idle frame.
    std::optional<svg::SVGElement> element;
    std::string label;
    bool isSelected = false;
    std::vector<TreeNodeSnapshot> children;
  };

  struct InspectorSnapshot {
    bool hasSelection = false;
    std::string titleText;
    std::optional<Box2d> bounds;
    std::optional<Transform2d> transform;
    std::vector<std::pair<std::string, std::string>> xmlAttributes;
    std::vector<std::pair<std::string, std::string>> computedStyle;
  };

  /// State for the transform edit currently in progress (one ImGui item can
  /// be active at a time, so a single slot suffices). Captured on item
  /// activation while the document is in sync; committed as one undo entry
  /// when the item deactivates after an edit.
  struct TransformEditState {
    svg::SVGElement element;           ///< Element being edited.
    TransformField field;              ///< Active field.
    int matrixIndex = 0;               ///< Matrix component index for `TransformField::Matrix`.
    const char* undoLabel = "";        ///< Undo timeline label for the completed edit.
    Transform2d startTransform;        ///< Local transform at activation.
    Transform2d currentTransform;      ///< Last transform queued via SetTransformCommand.
    std::optional<Box2d> startBounds;  ///< Document-space bounds at activation, if any.
    std::optional<DecomposedTransform> startDecomposed;  ///< Decomposition of `startTransform`.
    /// Stable locator so undo / source writeback survive document identity changes.
    std::optional<AttributeWritebackTarget> writebackTarget;
    /// Verbatim `transform=` source bytes at activation, restored on undo.
    std::optional<RcString> sourceTransformAttributeValue;
    double fieldValue = 0.0;  ///< Current value of the active scalar field.
    /// Raw matrix components being edited for `TransformField::Matrix`.
    std::array<double, 6> matrixValues{};
    bool changed = false;  ///< Whether any mutation was queued for this edit.
    /// Set when the edit deactivated on a frame without live app access;
    /// the commit is finalized on the next frame that has it.
    bool pendingCommit = false;
  };

  void captureTreeNode(const svg::SVGElement& element, std::span<const svg::SVGElement> selection,
                       TreeNodeSnapshot& out);
  void renderTreeNode(EditorApp* liveApp, const TreeNodeSnapshot& node, TreeViewState& state,
                      const IconTextureProvider& iconTextureProvider) const;

  /// Flip the persistent disclosure state for @p entityId (the model the tree
  /// chevron drives), shared by the click handler and the testing hook.
  void toggleTreeNodeExpanded(std::uint32_t entityId) const;

  /// Render the editable transform section (decomposed fields plus the raw
  /// matrix disclosure). Returns true if a mutation was queued.
  bool renderTransformPanel(EditorApp* liveApp);

  /// Render one decomposed-field DragFloat, wiring activation, write-back,
  /// and commit. Returns true if a mutation was queued.
  bool renderTransformFieldDrag(EditorApp* liveApp, TransformField field, const char* label,
                                float displayValue, bool canEdit, const char* undoLabel,
                                float dragSpeed, const char* format);

  /// Capture the edit baseline for @p field from the live element.
  void beginTransformEdit(EditorApp& liveApp, TransformField field, int matrixIndex,
                          const char* undoLabel);

  /// Apply a one-shot rotate-90 / flip transform to the single selected
  /// element and record one undo entry (Position region buttons, QA-F16).
  /// Reuses the same SetTransformCommand + undo + source-writeback path as
  /// \ref commitTransformEdit, so the locking / writeback semantics are
  /// unchanged. Returns true if a mutation was queued.
  bool applyDiscreteTransform(EditorApp& liveApp, DiscreteTransform kind);

  /// Compose the new local transform for the in-progress edit at @p value.
  [[nodiscard]] Transform2d composeFieldTransform(const TransformEditState& state,
                                                  double value) const;

  /// Queue a SetTransformCommand for the in-progress edit at @p value.
  bool applyTransformEdit(EditorApp& liveApp, double value);

  /// Record the single undo entry and source writeback for a completed edit,
  /// then clear the edit state. No-ops (state cleared) if nothing changed.
  void commitTransformEdit(EditorApp& liveApp);

  std::optional<TreeNodeSnapshot> treeSnapshot_;
  InspectorSnapshot inspectorSnapshot_;
  std::optional<TransformEditState> transformEdit_;

  /// Persistent tree-disclosure state, keyed by 32-bit entity id. A node is
  /// expanded iff present. Mutable because the tree renders from a const
  /// snapshot but owns its own view state (like imgui's former internal open
  /// state). Reset implicitly as entities change across document reloads.
  mutable std::unordered_set<std::uint32_t> treeExpandedEntities_;
};

}  // namespace donner::editor
