#pragma once
/// @file
///
/// `OverlayRenderer` draws editor chrome (currently selection path outlines)
/// directly into the renderer's existing framebuffer using
/// the canvas primitives `RendererInterface` already exposes. It is **not**
/// a separate compositing layer and **not** a fabricated SVG subtree —
/// chrome and document share one render target so there is no subpixel
/// drift between them.
///
/// See `docs/design_docs/editor.md` "OverlayRenderer uses direct canvas-
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
#include "donner/base/Path.h"
#include "donner/base/Transform.h"
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
  /// Capture only the combined selection bounds, skipping path extraction.
  CombinedBoundsOnly,
};

/// Frozen view of everything `OverlayRenderer::drawChromeWithTransform`
/// would normally read off the live registry: per-element path splines
/// transformed into document space, per-element AABBs in document space,
/// and the optional marquee rect.
///
/// Captured once via `OverlayRenderer::captureChromeSnapshot` while the
/// registry is safe to read (worker is idle), then handed to
/// `OverlayRenderer::drawChromeFromSnapshot` which is race-free —
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

  /// Transient "this element is locked, you can't select it" feedback. When present, the rejected
  /// (locked) element's outline is stroked in red with alpha scaled by `intensity` (1 → 0 as the
  /// flash fades). Captured from `SelectTool::lockedRejectionFlash()`.
  struct LockedFlash {
    /// Document-space path of the rejected (locked) element, sampled at capture time — same
    /// document-space convention as `PathItem::pathDoc`.
    Path pathDoc;
    /// Fade intensity in (0, 1]; scales the red stroke's alpha.
    float intensity = 0.0f;
  };
  /// The active locked-rejection flash, or nullopt when no element is being rejected.
  std::optional<LockedFlash> lockedFlash;

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
  /// back-compat shim — kept so worker-thread callers and existing
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
  /// always match the current DOM snapshot — same source of truth as
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
  /// selected element — MUST be called when the worker is idle or
  /// otherwise not mutating these components on the same registry.
  /// When `cullRectDoc` is present, path outlines, AABBs, and handles
  /// fully outside that document-space rect are skipped before draw.
  /// `CombinedBoundsOnly` skips selected path extraction and stores one
  /// combined bounds box for low-latency large-selection feedback.
  ///
  /// Design doc 0033 §M7. Returned snapshot is movable and self-
  /// contained: it holds no registry pointers and survives any
  /// subsequent registry mutation.
  [[nodiscard]] static SelectionChromeSnapshot captureChromeSnapshot(
      std::span<const svg::SVGElement> selection, const std::optional<Box2d>& marqueeRectDoc,
      const Transform2d& canvasFromDoc,
      const std::optional<SelectionChromeBoundsPreview>& activeBoundsPreview = std::nullopt,
      std::span<const svg::SVGElement> sourceHover = {},
      const std::optional<Box2d>& cullRectDoc = std::nullopt,
      SelectionChromeDetail selectionDetail = SelectionChromeDetail::Full,
      const Transform2d& representedDocumentFromLiveDocument = Transform2d(),
      const std::optional<LockedRejectionFlashInput>& lockedFlash = std::nullopt);

  /// Race-free chrome rasterize: reads only the snapshot, never the
  /// registry. Safe to call while the async-renderer worker is
  /// mid-render.
  ///
  /// Produces byte-identical pixels to
  /// `drawChromeWithTransform(selection, marqueeRectDoc, canvasFromDoc)`
  /// given the same input snapshot — pinned by
  /// `OverlayRendererTest.SnapshotProducesByteIdenticalPixels`.
  static void drawChromeFromSnapshot(svg::RendererInterface& renderer,
                                     const SelectionChromeSnapshot& snapshot);
};

}  // namespace donner::editor
