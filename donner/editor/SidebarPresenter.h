#pragma once
/// @file

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Length.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EmbeddedSvgIcon.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/StrokeMarkerPrefabs.h"
#include "donner/editor/ViewportState.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

struct EditorTheme;

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

/// Provenance shown beside a computed CSS value in the Inspector.
enum class InspectorStyleState : std::uint8_t {
  Unspecified,  ///< The serialized value did not include provenance metadata.
  Default,      ///< The computed value comes from the property's default.
  Set,          ///< The computed value was set by the active style cascade.
};

/// User-facing form of a computed CSS value and its provenance.
struct InspectorStyleDisplayValue {
  std::string value;  ///< CSS-shaped value without internal wrapper names.
  InspectorStyleState state = InspectorStyleState::Unspecified;  ///< Display provenance.
};

/// Convert the Inspector's serialized computed-style value to user-facing CSS.
///
/// Removes the snapshot's provenance suffix and internal `PaintServer(...)` or
/// `Color(...)` wrappers while preserving the actual CSS value.
///
/// @param serializedValue Value captured by the Inspector snapshot.
/// @return CSS-shaped value plus the captured provenance state.
[[nodiscard]] InspectorStyleDisplayValue FormatInspectorStyleValue(
    std::string_view serializedValue);

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
   * @return true if an inspector action queued a document mutation.
   */
  bool renderInspector(EditorApp* liveApp, const ViewportState& viewport,
                       const IconTextureProvider& iconTextureProvider = {});

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

  [[nodiscard]] std::span<const std::optional<ImU32>> inspectorComputedStyleSwatchesForTesting()
      const {
    return inspectorSnapshot_.computedStyleSwatches;
  }

  [[nodiscard]] const std::optional<Box2d>& inspectorBoundsForTesting() const {
    return inspectorSnapshot_.bounds;
  }

  [[nodiscard]] const std::optional<Transform2d>& inspectorTransformForTesting() const {
    return inspectorSnapshot_.transform;
  }

  /// Screen rectangle of the stroke-width increment button from the last inspector frame.
  [[nodiscard]] std::optional<Box2d> strokeIncrementRectForTesting() const {
    return strokeIncrementRect_;
  }
  [[nodiscard]] std::optional<Box2d> strokeDecrementRectForTesting() const {
    return strokeDecrementRect_;
  }
  /// Last rendered width value field, for drag and unit-preservation checks.
  [[nodiscard]] std::optional<Box2d> strokeWidthRectForTesting() const { return strokeWidthRect_; }
  /// Quick-width preset row bounds while the hybrid popup is open.
  [[nodiscard]] std::optional<Box2d> strokeWidthPresetRectForTesting(std::size_t index) const {
    return index < strokeWidthPresetRects_.size() ? strokeWidthPresetRects_[index] : std::nullopt;
  }

  /// Last rendered cap/join icon rectangle, for interaction and alignment checks.
  [[nodiscard]] std::optional<Box2d> strokeCapRectForTesting(std::size_t index) const {
    return index < strokeCapRects_.size() ? strokeCapRects_[index] : std::nullopt;
  }
  [[nodiscard]] std::optional<Box2d> strokeJoinRectForTesting(std::size_t index) const {
    return index < strokeJoinRects_.size() ? strokeJoinRects_[index] : std::nullopt;
  }
  /// Last rendered dashed-line toggle rectangle.
  [[nodiscard]] std::optional<Box2d> strokeDashToggleRectForTesting() const {
    return strokeDashToggleRect_;
  }
  /// Current dash-line preview and the three visual preset hit boxes.
  [[nodiscard]] std::optional<Box2d> strokeDashPreviewRectForTesting() const {
    return strokeDashPreviewRect_;
  }
  [[nodiscard]] std::optional<Box2d> strokeDashPresetRectForTesting(std::size_t index) const {
    return index < strokeDashPresetRects_.size() ? strokeDashPresetRects_[index] : std::nullopt;
  }
  /// Whether the current dashed style uses a pattern outside the visual presets.
  [[nodiscard]] bool strokeCustomDashSelectedForTesting() const {
    return strokeCustomDashSelected_;
  }
  /// Whether the miter limit is shown for the selected join style.
  [[nodiscard]] bool strokeMiterLimitVisibleForTesting() const {
    return inspectorSnapshot_.strokeLinejoin == 0 || inspectorSnapshot_.strokeLinejoin == 1 ||
           inspectorSnapshot_.strokeLinejoin == 4;
  }
  /// Last rendered sharp-join limit field, absent for rounded/beveled joins.
  [[nodiscard]] std::optional<Box2d> strokeMiterLimitRectForTesting() const {
    return strokeMiterLimitRect_;
  }
  [[nodiscard]] std::optional<Box2d> strokeMiterIncrementRectForTesting() const {
    return strokeMiterIncrementRect_;
  }
  [[nodiscard]] std::optional<Box2d> strokeMiterDecrementRectForTesting() const {
    return strokeMiterDecrementRect_;
  }
  /// Last rendered dash-offset field when its advanced row is visible.
  [[nodiscard]] std::optional<Box2d> strokeDashOffsetRectForTesting() const {
    return strokeDashOffsetRect_;
  }

  /// Whether the captured dash pattern fits the bounded text editor.
  [[nodiscard]] bool dashPatternEditableForTesting() const {
    return inspectorSnapshot_.strokeDasharray.size() < strokeDasharrayBuffer_.size();
  }

  /// Cached IDs offered by the marker selectors.
  [[nodiscard]] std::span<const std::string> markerIdsForTesting() const { return markerCacheIds_; }

  [[nodiscard]] std::optional<Box2d> markerDisclosureRectForTesting() const {
    return markerDisclosureRect_;
  }
  [[nodiscard]] std::optional<Box2d> markerPickerRectForTesting(std::size_t index) const {
    return index < markerPickerRects_.size() ? markerPickerRects_[index] : std::nullopt;
  }
  [[nodiscard]] std::optional<Box2d> markerPrefabRectForTesting(std::size_t index) const {
    return index < markerPrefabRects_.size() ? markerPrefabRects_[index] : std::nullopt;
  }

  /// Number of bounded marker scans performed by this presenter.
  [[nodiscard]] std::size_t markerScanCountForTesting() const { return markerScanCount_; }

  /// Submit a dash pattern through the same validation and style mutation path as the text field.
  bool submitDashPatternForTesting(EditorApp& app, std::string_view pattern) {
    return submitDashPattern(app, pattern);
  }
  /// Apply a dash offset in the currently captured SVG length unit.
  bool setStrokeDashOffsetForTesting(EditorApp& app, double value) {
    return setStrokeDashOffset(app, value);
  }

  /// Last rendered screen rectangle for one decomposed Transform field.
  /// Matrix cells are excluded because they are indexed separately.
  [[nodiscard]] std::optional<Box2d> transformFieldRectForTesting(TransformField field) const {
    if (field == TransformField::Matrix) {
      return std::nullopt;
    }
    return transformFieldRects_[static_cast<std::size_t>(field)];
  }

  /// Last rendered screen rectangle of one raw matrix component.
  [[nodiscard]] std::optional<Box2d> matrixFieldRectForTesting(int index) const {
    return matrixFieldRects_[static_cast<std::size_t>(index)];
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
    bool transformEditable = false;
    bool strokeEditable = false;
    Lengthd strokeWidth = Lengthd(1.0);
    int strokeLinecap = 0;
    int strokeLinejoin = 0;
    float strokeMiterlimit = 4.0f;
    std::string strokeDasharray = "none";
    Lengthd strokeDashoffset = Lengthd(0.0);
    std::string markerStart = "none";
    std::string markerEnd = "none";
    std::vector<std::string> markerIds;
    bool markerListTruncated = false;
    std::string titleText;
    std::optional<Box2d> bounds;
    std::optional<Transform2d> transform;
    std::vector<std::pair<std::string, std::string>> xmlAttributes;
    std::vector<std::pair<std::string, std::string>> computedStyle;
    std::vector<std::optional<ImU32>> computedStyleSwatches;
    std::vector<PathOperationAvailability> pathOperationAvailability;
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
  /// Populate stroke values and marker choices for the current selection.
  void captureStrokeSnapshot(const EditorApp& app, std::span<const svg::SVGElement> selection,
                             InspectorSnapshot& inspector);
  /// Re-scan marker IDs only when the source or root changed.
  void refreshMarkerCache(const EditorApp& app);
  void renderTreeNode(EditorApp* liveApp, const TreeNodeSnapshot& node, TreeViewState& state,
                      const IconTextureProvider& iconTextureProvider) const;

  /// Flip the persistent disclosure state for @p entityId (the model the tree
  /// chevron drives), shared by the click handler and the testing hook.
  void toggleTreeNodeExpanded(std::uint32_t entityId) const;

  /// Render the editable transform section (decomposed fields plus the raw
  /// matrix disclosure). Returns true if a mutation was queued.
  bool renderTransformPanel(EditorApp* liveApp);

  /// Render SVG stroke controls from the captured selection, queuing style mutations when idle.
  bool renderStrokeControlsPanel(EditorApp* liveApp,
                                 const IconTextureProvider& iconTextureProvider);

  enum class StrokeScalarField;
  struct StrokeRenderContext {
    EditorApp* app;
    const EditorTheme& theme;
    float rowStartX;
    bool canMutate;
    const IconTextureProvider& iconTextureProvider;
  };
  bool renderStrokeWidthRow(const StrokeRenderContext& context);
  bool renderStrokeWidthField(const StrokeRenderContext& context, const Lengthd& widthLength,
                              float* width);
  bool renderStrokeWidthStepper(const StrokeRenderContext& context, const Lengthd& widthLength,
                                float width);
  bool renderStrokeWidthPresetPopup(const StrokeRenderContext& context, const Lengthd& widthLength,
                                    bool fieldActivated);
  bool renderStrokeWidthPresetRow(const StrokeRenderContext& context, const Lengthd& widthLength,
                                  const IconTexture& texture, std::size_t index);
  bool renderStrokeCapRow(const StrokeRenderContext& context);
  bool renderStrokeJoinRow(const StrokeRenderContext& context);
  bool renderStrokeMiterRow(const StrokeRenderContext& context);
  bool renderStrokeMiterInputValue(const StrokeRenderContext& context, float miterlimit,
                                   bool inputChanged);
  bool renderStrokeMiterStepper(const StrokeRenderContext& context, const Box2d& field,
                                float miterlimit);
  bool renderStrokeDashSection(const StrokeRenderContext& context);
  bool renderDashPresetRow(const StrokeRenderContext& context,
                           std::span<const float> currentLengths);
  void renderDashPreviewRow(const StrokeRenderContext& context, std::span<const float> lengths,
                            std::string_view pattern);
  bool renderDashCustomEditor(const StrokeRenderContext& context, std::string_view pattern);
  bool renderDashOffsetRow(const StrokeRenderContext& context);
  bool renderStrokeMarkers(const StrokeRenderContext& context);
  bool renderStrokeMarkerPicker(const StrokeRenderContext& context, const char* label,
                                const char* property, const std::string& current);
  bool renderMarkerPrefabChoices(const StrokeRenderContext& context, const char* property,
                                 std::optional<StrokeMarkerPrefab> currentPrefab);
  bool renderDocumentMarkerChoices(const StrokeRenderContext& context, const char* property,
                                   const std::string& current);
  /// Track activation and release of the just-rendered scalar widget.
  void trackStrokeScalarItem(const StrokeRenderContext& context, StrokeScalarField field);

  /// Reject invalid SVG dash patterns before changing the selected elements' styles.
  bool submitDashPattern(EditorApp& liveApp, std::string_view pattern);
  /// Set the offset using the captured unit, including em and percent.
  bool setStrokeDashOffset(EditorApp& liveApp, double value);

  /// Queue one style change with a source-level undo checkpoint.
  bool applyStrokeStyle(EditorApp& liveApp, std::string_view property, std::string_view value,
                        std::string_view undoLabel);

  enum class StrokeScalarField { Width, MiterLimit, DashOffset };
  /// Capture/commit a continuous numeric edit as one source-level undo step.
  void beginStrokeScalarEdit(EditorApp& liveApp, StrokeScalarField field);
  void finishStrokeScalarEdit(EditorApp& liveApp, StrokeScalarField field);

  /// Render one decomposed numeric field, wiring activation, write-back, and
  /// commit. The field supports both drag adjustment and click-to-type.
  bool renderTransformFieldDrag(EditorApp* liveApp, TransformField field, const char* label,
                                float displayValue, bool canEdit, const char* undoLabel,
                                float dragSpeed, const char* format);

  /// Capture the edit baseline for @p field from the live element.
  void beginTransformEdit(EditorApp& liveApp, TransformField field, int matrixIndex,
                          const char* undoLabel);

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
  std::array<std::optional<Box2d>, 5> transformFieldRects_;
  std::array<std::optional<Box2d>, 6> matrixFieldRects_;
  std::optional<Box2d> strokeIncrementRect_;
  std::optional<Box2d> strokeDecrementRect_;
  std::optional<Box2d> strokeWidthRect_;
  std::array<std::optional<Box2d>, 8> strokeWidthPresetRects_;
  std::array<std::optional<Box2d>, 3> strokeCapRects_;
  std::array<std::optional<Box2d>, 5> strokeJoinRects_;
  std::optional<Box2d> strokeDashToggleRect_;
  std::optional<Box2d> strokeDashPreviewRect_;
  std::array<std::optional<Box2d>, 3> strokeDashPresetRects_;
  bool strokeCustomDashSelected_ = false;
  std::optional<Box2d> strokeMiterLimitRect_;
  std::optional<Box2d> strokeMiterIncrementRect_;
  std::optional<Box2d> strokeMiterDecrementRect_;
  std::optional<Box2d> strokeDashOffsetRect_;
  std::array<char, 128> strokeDasharrayBuffer_{};
  bool strokeDasharrayEditing_ = false;
  bool strokeDashDetailsOpen_ = false;
  std::string lastDashPattern_ = "4 2";
  std::string strokeDashError_;
  std::string strokeMiterError_;
  struct StrokeScalarEdit {
    StrokeScalarField field;
    std::string beforeSource;
    bool changed = false;
    bool pendingCommit = false;
  };
  std::optional<StrokeScalarEdit> strokeScalarEdit_;
  std::optional<svg::SVGElement> markerCacheRoot_;
  std::uint64_t markerCacheSourceVersion_ = 0;
  std::string markerCacheSourceText_;
  std::vector<std::string> markerCacheIds_;
  std::optional<Box2d> markerDisclosureRect_;
  std::array<std::optional<Box2d>, 2> markerPickerRects_{};
  std::array<std::optional<Box2d>, kStrokeMarkerPrefabOptions.size()> markerPrefabRects_{};
  bool markerCacheTruncated_ = false;
  std::size_t markerScanCount_ = 0;

  /// Persistent tree-disclosure state, keyed by 32-bit entity id. A node is
  /// expanded iff present. Mutable because the tree renders from a const
  /// snapshot but owns its own view state (like imgui's former internal open
  /// state). Reset implicitly as entities change across document reloads.
  mutable std::unordered_set<std::uint32_t> treeExpandedEntities_;
};

/// Every inspector path-operation button icon, for the shell's startup prewarm
/// batch. These first appear when a selection makes the boolean-op row
/// available, so batching them with the boot icons keeps that first selection
/// from stalling on a run of GPU readbacks.
[[nodiscard]] std::span<const EmbeddedSvgIconRequest> SidebarIconPrewarmRequests();

/// Stroke-cap and stroke-join previews drawn by the SVG renderer, in button order.
enum class StrokePreviewIcon : std::uint8_t {
  ButtCap,
  RoundCap,
  SquareCap,
  MiterJoin,
  RoundJoin,
  BevelJoin,
};

inline constexpr std::array<StrokePreviewIcon, 6> kStrokePreviewIcons = {
    StrokePreviewIcon::ButtCap,   StrokePreviewIcon::RoundCap,  StrokePreviewIcon::SquareCap,
    StrokePreviewIcon::MiterJoin, StrokePreviewIcon::RoundJoin, StrokePreviewIcon::BevelJoin,
};

/// Common user-unit widths represented at one SVG unit per logical pixel in the preset sprite.
inline constexpr std::array<float, 8> kStrokeWidthPresetValues = {0.5f, 1.0f, 2.0f, 3.0f,
                                                                  4.0f, 6.0f, 8.0f, 12.0f};

/// One SVG sprite with one true-scale stroke sample for each quick-width preset.
[[nodiscard]] std::span<const unsigned char> StrokeWidthPresetSvg();

/// Stable uploaded texture key for the stroke-width preset sprite.
[[nodiscard]] std::uint64_t StrokeWidthPresetTextureKey();

/// Embedded SVG source whose actual stroke property produces this preview.
[[nodiscard]] std::span<const unsigned char> StrokePreviewIconSvg(StrokePreviewIcon icon);

/// Unique UI texture key for a rendered stroke preview.
[[nodiscard]] std::uint64_t StrokePreviewIconTextureKey(StrokePreviewIcon icon);

/// The path operations the inspector shows a button for, in button order.
inline constexpr std::array<PathOperationKind, 4> kInspectorPathOperations = {
    PathOperationKind::Union,
    PathOperationKind::Intersect,
    PathOperationKind::SubtractFront,
    PathOperationKind::Exclude,
};

/// Embedded SVG source for @p operation's button icon. Every operation the
/// inspector shows a button for maps to its own artwork, which the editor
/// rasterizes through Donner rather than shipping a baked bitmap or a font
/// glyph.
///
/// @param operation Path operation whose icon artwork is wanted.
[[nodiscard]] std::span<const unsigned char> PathOperationIconSvg(PathOperationKind operation);

/// Presentation-texture cache key for @p operation's button icon. Distinct per
/// operation so two buttons never share an uploaded texture.
///
/// @param operation Path operation whose icon texture key is wanted.
[[nodiscard]] std::uint64_t PathOperationIconTextureKey(PathOperationKind operation);

}  // namespace donner::editor
