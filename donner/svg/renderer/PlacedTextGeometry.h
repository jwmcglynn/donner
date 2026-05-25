#pragma once
/// @file
/// Shared, backend-agnostic text geometry for `RendererInterface::drawText`.
///
/// `RendererTinySkia::drawText` and `RendererGeode::drawText` historically each
/// re-derived per-glyph placement (outline → stretch → translate → rotate),
/// which let the two copies drift (see docs/design_docs/0038). This header is
/// the shared placement layer both backends consume so the geometry is computed
/// once. It encodes tiny-skia's behavior verbatim (tiny-skia is the parity
/// reference); geode converges to it.
///
/// Pure geometry only: inputs/outputs are `donner::Path` / `Transform2d` /
/// `TextEngine` — no backend paint types. No allocation beyond what `Path`
/// itself does; no exceptions.

#include <span>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Path.h"
#include "donner/base/RelativeLengthMetrics.h"
#include "donner/base/Transform.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/text/TextEngine.h"
#include "donner/svg/text/TextTypes.h"

namespace donner::svg {

/**
 * @brief Apply an affine transform to every point of a path.
 *
 * Backend-agnostic replacement for the byte-identical `transformPath` free
 * functions that previously lived in each renderer TU.
 *
 * @param path The source path.
 * @param transform The transform to apply to each point.
 * @return A new path with all points transformed.
 */
Path transformPath(const Path& path, const Transform2d& transform);

/**
 * @brief Build the placed outline for a single glyph in document space.
 *
 * Encodes tiny-skia's exact placement sequence:
 *   1. `glyphOutline(font, glyph.glyphIndex, scale * glyph.fontSizeScale)`,
 *   2. stretch the **raw outline** by `Scale(stretchScaleX, stretchScaleY)`
 *      (only when either differs from 1),
 *   3. position via `Rotate(rotateDegrees) * Translate(xPosition, yPosition)`.
 *
 * Returns an empty path for `.notdef` (glyphIndex 0) or when the font has no
 * vector outline for the glyph (e.g. bitmap-only fonts) — callers handle those
 * cases (skip / bitmap path) exactly as before.
 *
 * @param textEngine Engine providing glyph outlines.
 * @param font Font handle for the run.
 * @param glyph The positioned glyph (carries position, rotation, stretch).
 * @param scale Per-run pixel-height scale (`scaleForPixelHeight(font, sizePx)`).
 * @return The placed outline path in the text element's local space.
 */
Path placedGlyphOutline(const TextEngine& textEngine, FontHandle font, const TextGlyph& glyph,
                        float scale);

/**
 * @brief Compute the text element's bounding box for `objectBoundingBox` paint.
 *
 * Encodes tiny-skia's computation: the union over rendered glyphs of em-box
 * cells — horizontally `[xPosition, xPosition + xAdvance]`, vertically
 * `[yPosition - ascent*scale, yPosition - descent*scale]` (font v-metrics, not
 * the raw font size), per the SVG spec for text `objectBoundingBox`. A `tspan`
 * has no bbox of its own, so gradient/pattern paint on a span maps through this
 * element-level box.
 *
 * Returns an empty (default) `Box2d` when there are no rendered glyphs.
 *
 * @param textEngine Engine providing per-run scale + font v-metrics.
 * @param runs Positioned layout runs.
 * @param spans Per-span styles (for per-span font-size overrides).
 * @param viewBox Viewport box for length resolution.
 * @param fontMetrics Font metrics for length resolution.
 * @param fontSizePx Element-level resolved font size in pixels.
 * @return The text bounding box in the element's local space.
 */
Box2d computeTextBounds(const TextEngine& textEngine, const std::vector<TextRun>& runs,
                        std::span<const components::ComputedTextComponent::TextSpan> spans,
                        const Box2d& viewBox, const FontMetrics& fontMetrics, float fontSizePx);

}  // namespace donner::svg
