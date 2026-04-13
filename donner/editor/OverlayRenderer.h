#pragma once
/// @file
///
/// `OverlayRenderer` draws editor chrome (selection bounds, future tool
/// handles, etc.) directly into the renderer's existing frame buffer using
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
  /// selection.
  static void drawChrome(svg::Renderer& renderer, const EditorApp& editor);

  /// Overload that takes a selection snapshot directly. Used by the
  /// async render worker, which captures the selection at render-
  /// request time so the draw path doesn't touch `EditorApp` (which
  /// lives on the UI thread).
  static void drawChrome(svg::Renderer& renderer,
                         const std::optional<svg::SVGElement>& selection);
};

}  // namespace donner::editor
