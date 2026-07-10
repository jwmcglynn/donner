#pragma once
/// @file
///
/// `OverlayRenderer` draws editor chrome (currently selection path outlines)
/// directly into the renderer's existing framebuffer using
/// the canvas primitives `RendererInterface` already exposes. It is **not**
/// a separate compositing layer and **not** a fabricated SVG subtree -
/// chrome and document share one render target so there is no subpixel
/// drift between them.
///
/// See `docs/design_docs/0020-editor.md` "OverlayRenderer uses direct canvas-
/// style Renderer calls" for the architectural rationale and the primitive
/// policy.
///
/// Usage:
///
/// ```cpp
/// renderer.draw(editor.document().document());
/// donner::editor::OverlayRenderer::drawChrome(renderer, editor);
/// const auto bitmap = renderer.takeSnapshot();  // contains chrome
/// ```
///
/// Must be called **after** `Renderer::draw(document)` (which has its own
/// internal `beginFrame` / `endFrame` cycle) and **before**
/// `Renderer::takeSnapshot()`. The renderer's frame pixmap survives across
/// `endFrame` so additional canvas commands compose onto it directly.

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/FillRule.h"
#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/css/Color.h"
#include "donner/svg/SVGElement.h"

namespace donner::svg {
class RendererInterface;
}

namespace donner::editor {

class EditorApp;

/// Active transform bounds chrome. Used while rotating to draw the
/// gesture-start selection box transformed into the current document space.
struct SelectionChromeBoundsPreview {
  /// Selection AABB captured when the gesture started.
  Box2d startBoundsDoc;
  /// Current transform from gesture-start document space to active document space.
  Transform2d documentFromStartDocument = Transform2d();
};

/// Locked-rejection flash input for `OverlayRenderer::captureChromeSnapshot`. Carries the rejected
/// (locked) element whose outline should flash red plus the current fade intensity. Kept as a
/// dedicated struct (rather than a dependency on `SelectTool::LockedRejectionFlash`) so the overlay
/// renderer stays decoupled from the tool layer.
struct LockedRejectionFlashInput {
  /// The element whose selection was rejected because it (or an ancestor group) is locked.
  svg::SVGElement element;
  /// Fade intensity in (0, 1]; scales the red stroke's alpha at draw time.
  float intensity = 0.0f;
};

/// Detail level used when capturing selection chrome.
enum class SelectionChromeDetail {
  /// Capture visible path outlines plus selection bounds.
  Full,
  /// Capture visible path outlines only, skipping selection bounds and transform handles.
  PathOutlinesOnly,
  /// Capture only the combined selection bounds, skipping path extraction.
  CombinedBoundsOnly,
  /// Skip selection geometry entirely. Dedicated editing chrome (for example
  /// a text caret and session frame) is attached to the snapshot separately.
  EditingChromeOnly,
};

/// Frozen view of everything `OverlayRenderer::drawChromeWithTransform`
/// would normally read off the live registry: per-element path splines
/// transformed into document space, per-element AABBs in document space,
/// and the optional marquee rect.
///
/// Captured once via `OverlayRenderer::captureChromeSnapshot` while the
/// registry is safe to read (worker is idle), then handed to
/// `OverlayRenderer::drawChromeFromSnapshot` which is race-free -
/// it touches only the snapshot, never the registry.
///
/// Design doc 0033 §M7. Lets the chrome rasterize run while the worker
/// is mid-render, so the editor can paint selection chrome in a frame
/// that the worker hasn't returned a result for yet.
struct SelectionChromeSnapshot {
  /// Four document-space corners of an oriented selection box.
  struct OrientedBox {
    std::array<Vector2d, 4> cornersDoc;
  };

  /// One entry per renderable geometry leaf in the selection (groups
  /// are expanded into their geometry descendants, same as the live
  /// path). Empty when nothing's selected.
  struct PathItem {
    /// Document-space path data sampled at capture time. Keeping the path
    /// in document space lets chrome stroke width depend only on viewport
    /// scale, not on the selected element's own scale/rotation transform.
    Path pathDoc;
    /// True when the selected element is hidden by `display:none`. The path outline still renders
    /// as an editable placeholder, but uses a dimmer stroke than visible selections.
    bool displayNone = false;
  };
  std::vector<PathItem> paths;
  /// Transient source-hover path outlines. Drawn as soft hover chrome before selection chrome.
  std::vector<PathItem> hoverPaths;

  /// Document-space Bezier handle guide line for selected SVG paths.
  struct PathControlLine {
    /// Anchor endpoint of the control line.
    Vector2d anchorDoc = Vector2d();
    /// Control-point endpoint of the control line.
    Vector2d controlDoc = Vector2d();
  };

  /// Anchor squares for selected SVG path vertices, sized in document units at capture time.
  std::vector<Box2d> pathAnchorBoxesDoc;
  /// Control-point guide lines for selected SVG paths.
  std::vector<PathControlLine> pathControlLinesDoc;
  /// Control-point squares for selected SVG paths, sized in document units at capture time.
  std::vector<Box2d> pathControlPointBoxesDoc;

  /// Transient "this element is locked, you can't select it" feedback. When present, the rejected
  /// (locked) element's outline is stroked in red with alpha scaled by `intensity` (1 → 0 as the
  /// flash fades). Captured from `SelectTool::lockedRejectionFlash()`.
  struct LockedFlash {
    /// Document-space path of the rejected (locked) element, sampled at capture time - same
    /// document-space convention as `PathItem::pathDoc`.
    Path pathDoc;
    /// Fade intensity in (0, 1]; scales the red stroke's alpha.
    float intensity = 0.0f;
  };
  /// The active locked-rejection flash, or nullopt when no element is being rejected.
  std::optional<LockedFlash> lockedFlash;

  /// Live document-geometry preview of the path the Pen tool is actively
  /// authoring or point-editing. Drawn beneath the selection chrome with the
  /// path's own resolved solid paint, so the presented pixels for the edited
  /// path come from the same post-flush DOM capture as the chrome - they never
  /// wait for the async raster of the new geometry. While this is present the
  /// presenter suppresses the path's stale composited layer tile
  /// (`ShouldPresentCompositedTile`), otherwise the old geometry would show
  /// through underneath the preview.
  ///
  /// Only solid (or none) fill/stroke paints are representable; capture skips
  /// the preview (leaving nullopt) for gradients/patterns, `currentColor`, or
  /// elements carrying filter/clip-path/mask, and the presenter then falls
  /// back to the normal composited raster. The preview composes above all
  /// cached tiles, which matches pen authoring (new paths are appended last in
  /// paint order).
  struct LivePathPreview {
    /// Entity of the previewed path - the presenter suppresses this entity's
    /// composited layer tile while the preview is drawn.
    Entity entity = entt::null;
    /// Document-space path geometry sampled at capture time.
    Path pathDoc;
    /// Fill rule for the preview fill.
    FillRule fillRule = FillRule::NonZero;
    /// Resolved solid fill color, or nullopt for `fill: none`.
    std::optional<css::RGBA> fillColor;
    /// Resolved solid stroke color, or nullopt for `stroke: none`.
    std::optional<css::RGBA> strokeColor;
    /// Stroke width in document units.
    double strokeWidthDoc = 1.0;
    /// `opacity` (group opacity) multiplier.
    double opacity = 1.0;
    /// `fill-opacity` multiplier.
    double fillOpacity = 1.0;
    /// `stroke-opacity` multiplier.
    double strokeOpacity = 1.0;
  };
  /// The active pen live-geometry preview, or nullopt when the Pen tool is not
  /// editing a path (or its paint is not representable as a solid preview).
  std::optional<LivePathPreview> livePathPreview;

  /// Rubber-band preview of the segment the Pen tool would commit at the
  /// current pointer (document space). Stroked with the control-line chrome
  /// style so the pending segment reads as guidance, not committed geometry.
  std::optional<Path> penPreviewSegmentDoc;
  /// Close-path hover affordance: when set, the first anchor at this document
  /// point is highlighted (the pointer is within closing range).
  std::optional<Vector2d> penCloseAffordanceDoc;

  /// Text-editing caret for the in-canvas text session: document-space
  /// endpoints of the caret bar (top, bottom).
  struct TextCaret {
    Vector2d topDoc;
    Vector2d bottomDoc;

    bool operator==(const TextCaret&) const = default;
  };
  /// The active text-editing caret, or nullopt when no text session is open.
  std::optional<TextCaret> textCaretDoc;
  /// Session frame for the active text-editing session: local TL, TR, BR, BL
  /// corners mapped through the text's transform. An ORIENTED quad - after a
  /// rotate it stays aligned to the text's rotation, never the axis-aligned
  /// envelope. Corner resize/rotate handles are drawn at these corners.
  std::optional<std::array<Vector2d, 4>> textFrameCornersDoc;
  /// Opacity of the text frame and its handles. Point text animates this
  /// value after pointer movement and typing; box text remains at 1.
  float textFrameOpacity = 1.0f;

  /// Drag-to-create text-box preview: the live rectangle the text tool is
  /// dragging out, plus the first baseline it would create and an I-beam
  /// marker at the future caret position. Drawn as dedicated text-box chrome
  /// (crisp frame + guidance baseline + I-beam), visually distinct from the
  /// selection marquee's translucent fill + white outline.
  struct TextBoxDragPreview {
    /// The live drag rectangle.
    Box2d boxDoc;
    /// First-baseline segment endpoints.
    Vector2d baselineStartDoc;
    Vector2d baselineEndDoc;
    /// I-beam bar endpoints at the future caret position (top, bottom).
    Vector2d ibeamTopDoc;
    Vector2d ibeamBottomDoc;

    bool operator==(const TextBoxDragPreview&) const = default;
  };
  /// The active drag-to-create preview, or nullopt when the text tool is not
  /// dragging out a box.
  std::optional<TextBoxDragPreview> textBoxDragPreviewDoc;

  /// One baseline segment of a selected text run, in document space.
  struct TextBaseline {
    /// Baseline start (pen position of the line's first glyph).
    Vector2d startDoc;
    /// Baseline end (advance end of the line's last glyph).
    Vector2d endDoc;

    bool operator==(const TextBaseline&) const = default;
  };
  /// Baseline underlay for selected text: one document-space segment per
  /// laid-out text line, drawn beneath the selection chrome so the span of
  /// each line reads at a glance.
  std::vector<TextBaseline> textBaselinesDoc;

  /// Per-element AABBs in document space (from
  /// `SnapshotSelectionWorldBounds`). Drawn with `canvasFromDoc`
  /// applied at compose time so they line up with the rendered
  /// content for the same DOM frame.
  std::vector<Box2d> aabbsDoc;
  /// Bounds for the transient source-hover elements.
  std::vector<Box2d> hoverAabbsDoc;

  /// Optional marquee rectangle in document space.
  std::optional<Box2d> marqueeDoc;

  /// Optional oriented bounds drawn instead of axis-aligned AABBs while
  /// a rotation gesture is active.
  std::optional<OrientedBox> orientedBoundsDoc;

  /// Corner resize handles in document space. Empty when there is no
  /// selection AABB.
  std::vector<Box2d> handleBoxesDoc;

  /// `canvasFromDoc` at capture time. The draw phase needs this for
  /// drawing AABBs and the marquee (both live in doc space).
  Transform2d canvasFromDoc;

  /// World-space stroke widths derived from the snapshot's
  /// `canvasFromDoc` scale, pre-computed so the draw phase doesn't
  /// have to recompute anything that depends on registry state.
  double selectionStrokeWidthWorld = 0.0;
  double hoverStrokeWidthWorld = 0.0;
  double marqueeStrokeWidthWorld = 0.0;
};

class OverlayRenderer {
public:
  /// Draw all editor chrome layers for the current state of `editor` into
  /// `renderer`'s active frame. No-op if there is no document or no
  /// selection. Pulls the `canvasFromDocument` transform from the editor's
  /// document so chrome lands on the same pixels as the rendered content,
  /// regardless of canvas/viewBox aspect mismatch.
  static void drawChrome(svg::RendererInterface& renderer, const EditorApp& editor);

  /// Overload that takes a selection snapshot directly. Single-element
  /// back-compat shim - kept so worker-thread callers and existing
  /// tests don't have to switch to spans. Identity `canvasFromDoc`.
  static void drawChrome(svg::RendererInterface& renderer,
                         const std::optional<svg::SVGElement>& selection);

  /// Lower-level single-element entry: draw chrome for `selection`
  /// (or none) using `canvasFromDoc`. Kept for back-compat with the
  /// existing single-select call sites.
  static void drawChromeWithTransform(svg::RendererInterface& renderer,
                                      const std::optional<svg::SVGElement>& selection,
                                      const Transform2d& canvasFromDoc);

  /// Multi-element entry: draw path outlines for every selected
  /// element, selection AABBs, and the optional marquee rect into
  /// `renderer`'s active frame using `canvasFromDoc`.
  ///
  /// This renderer-backed path is kept for pixel tests and non-editor callers. The live editor
  /// presentation path captures the same snapshot, then draws it immediately through
  /// `RenderPanePresenter` to avoid allocating and uploading a full overlay texture.
  ///
  /// AABBs are computed inline from `selection` (via
  /// `SnapshotSelectionWorldBounds`) at overlay-draw time so they
  /// always match the current DOM snapshot - same source of truth as
  /// the per-element path outlines. This avoids the frame-lag-shear
  /// that happened when AABBs came from a separately-promoted cache
  /// while the path outlines came from the live DOM.
  ///
  /// @param selection Selected elements whose per-element path outlines
  ///   + AABBs should be drawn.
  /// @param marqueeRectDoc Optional marquee rect in document space;
  ///   drawn as a filled + stroked rectangle matching the prior
  ///   ImGui chrome style.
  /// @param canvasFromDoc Maps document coordinates into canvas pixels.
  /// @param cullRectDoc Optional document-space cull rect. Chrome fully outside this rect is
  ///   skipped before draw.
  /// @param selectionDetail Detail level for selected-element chrome.
  static void drawChromeWithTransform(
      svg::RendererInterface& renderer, std::span<const svg::SVGElement> selection,
      const std::optional<Box2d>& marqueeRectDoc, const Transform2d& canvasFromDoc,
      const std::optional<SelectionChromeBoundsPreview>& activeBoundsPreview = std::nullopt,
      std::span<const svg::SVGElement> sourceHover = {},
      const std::optional<Box2d>& cullRectDoc = std::nullopt,
      SelectionChromeDetail selectionDetail = SelectionChromeDetail::Full,
      const Transform2d& representedDocumentFromLiveDocument = Transform2d());

  /// Back-compat overload without marquee. Kept for existing callers
  /// that don't need a marquee rect (older tests, worker-thread
  /// helpers). Path outlines + selection AABBs are still drawn.
  static void drawChromeWithTransform(svg::RendererInterface& renderer,
                                      std::span<const svg::SVGElement> selection,
                                      const Transform2d& canvasFromDoc);

  /// Build a `SelectionChromeSnapshot` for the given selection +
  /// marquee + canvas transform. Reads `computedSpline`,
  /// `elementFromWorld`, and `SnapshotSelectionWorldBounds` for every
  /// selected element - MUST be called when the worker is idle or
  /// otherwise not mutating these components on the same registry.
  /// When `cullRectDoc` is present, path outlines, AABBs, and handles
  /// fully outside that document-space rect are skipped before draw.
  /// `CombinedBoundsOnly` skips selected path extraction and stores one
  /// combined bounds box for low-latency large-selection feedback.
  /// @param devicePixelRatio Viewport framebuffer scale used to convert
  ///   logical UI stroke widths into device pixels.
  ///
  /// Design doc 0033 §M7. Returned snapshot is movable and self-
  /// contained: it holds no registry pointers and survives any
  /// subsequent registry mutation.
  /// @param livePathPreviewElement When set, the element the Pen tool is
  ///   actively editing: its live document-space geometry and resolved solid
  ///   paint are captured into `SelectionChromeSnapshot::livePathPreview` so
  ///   the draw phase can present the edited path from the same DOM capture as
  ///   the chrome. Skipped (nullopt in the snapshot) when the element's paint
  ///   cannot be represented as a solid preview.
  [[nodiscard]] static SelectionChromeSnapshot captureChromeSnapshot(
      std::span<const svg::SVGElement> selection, const std::optional<Box2d>& marqueeRectDoc,
      const Transform2d& canvasFromDoc,
      const std::optional<SelectionChromeBoundsPreview>& activeBoundsPreview = std::nullopt,
      std::span<const svg::SVGElement> sourceHover = {},
      const std::optional<Box2d>& cullRectDoc = std::nullopt,
      SelectionChromeDetail selectionDetail = SelectionChromeDetail::Full,
      const Transform2d& representedDocumentFromLiveDocument = Transform2d(),
      const std::optional<LockedRejectionFlashInput>& lockedFlash = std::nullopt,
      double devicePixelRatio = 1.0,
      const std::optional<svg::SVGElement>& livePathPreviewElement = std::nullopt);

  /// Race-free chrome rasterize: reads only the snapshot, never the
  /// registry. Safe to call while the async-renderer worker is
  /// mid-render.
  ///
  /// Produces byte-identical pixels to
  /// `drawChromeWithTransform(selection, marqueeRectDoc, canvasFromDoc)`
  /// given the same input snapshot - pinned by
  /// `OverlayRendererTest.SnapshotProducesByteIdenticalPixels`.
  static void drawChromeFromSnapshot(svg::RendererInterface& renderer,
                                     const SelectionChromeSnapshot& snapshot);
};

}  // namespace donner::editor
