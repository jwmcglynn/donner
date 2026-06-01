#pragma once
/// @file

#include "donner/base/Vector2.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg {

/**
 * Rasterize a single \ref SVGElement and its descendant subtree to an RGBA
 * bitmap fitted into a target box.
 *
 * This is the "isolated preview" entrypoint used by editor UI (the Layers panel
 * thumbnails): it renders just the given element + its descendants through the
 * real Donner renderer (\ref RendererDriver over the element's entity range),
 * scaled and centered to fit @p sizePx with a transparent background. The
 * element's own paint and transform are honored, but the ancestor chain's clips
 * and opacity are intentionally ignored so the preview shows the element's own
 * artwork rather than how it happens to be clipped in context.
 *
 * The element's owning document is prepared for rendering (\ref
 * RendererUtils::prepareDocumentForRendering) under write access, so this must
 * only be called when no other thread holds the document (e.g. the editor's
 * idle `refreshSnapshot`, not mid-render).
 *
 * @param element Element whose subtree should be rasterized. May be any
 *   element type; non-renderable or empty subtrees yield an empty bitmap.
 * @param sizePx Target bitmap dimensions in device pixels. Both axes must be
 *   positive; otherwise an empty bitmap is returned.
 * @return The rendered RGBA bitmap (premultiplied alpha, transparent
 *   background), or an empty \ref RendererBitmap when the element has no
 *   renderable geometry or the inputs are degenerate.
 */
[[nodiscard]] RendererBitmap RenderElementToBitmap(SVGElement element, Vector2i sizePx);

}  // namespace donner::svg
