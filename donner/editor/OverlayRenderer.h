#pragma once
/// @file
///
/// `OverlayRenderer` draws editor chrome (currently selection path outlines)
/// directly into the renderer's existing frame buffer using
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

#include <optional>
#include <span>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/svg/SVGElement.h"

namespace donner::svg {
class Renderer;
}

namespace donner::editor {

class EditorApp;

class OverlayRenderer {
public:
  /// Draw all editor chrome layers for the current state of `editor` into
  /// `renderer`'s active frame. No-op if there is no document or no
  /// selection. Pulls the `canvasFromDocument` transform from the editor's
  /// document so chrome lands on the same pixels as the rendered content,
  /// regardless of canvas/viewBox aspect mismatch.
  static void drawChrome(svg::Renderer& renderer, const EditorApp& editor);

  /// Overload that takes a selection snapshot directly. Single-element
  /// back-compat shim — kept so worker-thread callers and existing
  /// tests don't have to switch to spans. Identity `canvasFromDoc`.
  static void drawChrome(svg::Renderer& renderer, const std::optional<svg::SVGElement>& selection);

  /// Lower-level single-element entry: draw chrome for `selection`
  /// (or none) using `canvasFromDoc`. Kept for back-compat with the
  /// existing single-select call sites.
  static void drawChromeWithTransform(svg::Renderer& renderer,
                                      const std::optional<svg::SVGElement>& selection,
                                      const Transform2d& canvasFromDoc);

  /// Multi-element entry: draw path outlines for every selected
  /// element, selection AABBs, and the optional marquee rect into
  /// `renderer`'s active frame using `canvasFromDoc`.
  ///
  /// All chrome lives in this one rasterized overlay layer. The earlier
  /// "two-path" design that drew AABBs + marquee directly via
  /// ImGui's draw list in `RenderPanePresenter` was folded into here
  /// — Geode can optimize the whole layer end-to-end (a single
  /// invalidation envelope, a single GPU upload) once it lands.
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
  static void drawChromeWithTransform(svg::Renderer& renderer,
                                      std::span<const svg::SVGElement> selection,
                                      const std::optional<Box2d>& marqueeRectDoc,
                                      const Transform2d& canvasFromDoc);

  /// Back-compat overload without marquee. Kept for existing callers
  /// that don't need a marquee rect (older tests, worker-thread
  /// helpers). Path outlines + selection AABBs are still drawn.
  static void drawChromeWithTransform(svg::Renderer& renderer,
                                      std::span<const svg::SVGElement> selection,
                                      const Transform2d& canvasFromDoc);
};

}  // namespace donner::editor
