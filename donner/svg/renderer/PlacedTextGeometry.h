#pragma once
/// @file
/// Shared, backend-agnostic text geometry for `RendererInterface::drawText`.
///
/// `RendererTinySkia::drawText` and `RendererGeode::drawText` historically each
/// re-derived per-glyph placement (outline → stretch → translate → rotate),
/// which let the two copies drift. This header is
/// the shared placement layer both backends consume so the geometry is computed
/// once. It encodes tiny-skia's behavior verbatim (tiny-skia is the parity
/// reference); geode converges to it.
///
/// Pure geometry only: inputs/outputs are `donner::Path` / `Transform2d` /
/// `TextEngine` - no backend paint types. No allocation beyond what `Path`
/// itself does; no exceptions.

#include <functional>
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

struct TextParams;

/// Immutable layout slice and full-element bounds prepared once for a text effect owner.
struct PreparedTextDraw {
  std::vector<TextRun> runs;
  Box2d elementBounds;
};

/// Mutable per-call run copy after effect ownership and visibility filtering.
struct TextDrawGeometry {
  std::vector<TextRun> runs;
  Box2d elementBounds;
};

/// Convert renderer-facing text parameters to the shared layout engine's input.
[[nodiscard]] TextLayoutParams ToTextLayoutParams(const TextParams& params);

/// Resolve one backend draw from a shared element preparation or the direct-caller fallback.
[[nodiscard]] TextDrawGeometry PrepareTextDrawGeometry(
    Registry& registry, const components::ComputedTextComponent& text, const TextParams& params,
    TextEngine& textEngine);

/**
 * @brief Apply an affine transform to every point of a path.
 *
 * Backend-agnostic replacement for the byte-identical `TransformPath` free
 * functions that previously lived in each renderer TU.
 *
 * @param path The source path.
 * @param transform The transform to apply to each point.
 * @return A new path with all points transformed.
 */
Path TransformPath(const Path& path, const Transform2d& transform);

/**
 * @brief A glyph's outline split into the part that depends only on the glyph
 *   and the part that depends only on where the glyph sits in the run.
 *
 * The two compose back into the placed outline: `TransformPath(outline,
 * glyphFromLocal)` maps to the same place `PlacedGlyphOutline` does. It is not
 * the same arithmetic - that one composes the placement into a single affine -
 * so the two agree as mappings, not bit for bit, whenever a rotation is
 * present.
 *
 * Splitting them lets a renderer key a cache on glyph identity (font, glyph
 * index, effective scale, stretch, rotation) and reuse one outline for every
 * occurrence, carrying the per-occurrence placement as a transform instead of
 * re-deriving and re-transforming the outline for each one.
 */
struct GlyphOutlineAndPlacement {
  /// Raw glyph outline at the glyph's effective scale, with the lengthAdjust
  /// stretch and the per-glyph rotation already baked in. Empty for `.notdef`
  /// and for glyphs with no vector outline.
  Path outline;
  /// Pure translation placing `outline` at its baseline position in the text
  /// element's local space.
  Transform2d glyphFromLocal;
};

/**
 * @brief The affine that places a glyph's unplaced outline at its baseline
 *   position in the text element's local space.
 *
 * A pure translation to `(xPosition, yPosition)`. Per-glyph rotation is NOT
 * here: it is baked into the outline by \ref UnplacedGlyphOutline, so a
 * renderer that rasterises in the outline's own space keeps that space aligned
 * with the pixel grid. Depends only on the glyph's position, so a renderer that
 * caches outlines by glyph identity can compute this per occurrence without
 * touching the font backend.
 *
 * @param glyph The positioned glyph.
 * @return The placement transform.
 */
Transform2d GlyphPlacementTransform(const TextGlyph& glyph);

/**
 * @brief Build a glyph's unplaced outline and its placement transform.
 *
 * Encodes tiny-skia's placement inputs, split at the point where the
 * glyph-identity-dependent work ends:
 *   1. `glyphOutline(font, glyph.glyphIndex, scale * glyph.fontSizeScale)`,
 *   2. stretch the **raw outline** by `Scale(stretchScaleX, stretchScaleY)`
 *      (only when either differs from 1),
 *   3. rotate by `rotateDegrees` about the glyph's own origin -- these three
 *      produce `outline`,
 *   4. `Translate(xPosition, yPosition)` -- this is `glyphFromLocal`.
 *
 * Steps 1 to 3 depend only on (font, glyph index, `scale * fontSizeScale`,
 * stretch, rotation); step 4 depends only on the glyph's position.
 *
 * @param textEngine Engine providing glyph outlines.
 * @param font Font handle for the run.
 * @param glyph The positioned glyph (carries position, rotation, stretch).
 * @param scale Per-run pixel-height scale (`scaleForPixelHeight(font, sizePx)`).
 * @return The unplaced outline plus its placement transform.
 */
GlyphOutlineAndPlacement UnplacedGlyphOutline(const TextEngine& textEngine, FontHandle font,
                                              const TextGlyph& glyph, float scale);

/**
 * @brief Build the placed outline for a single glyph in document space.
 *
 * Encodes tiny-skia's exact placement sequence: stretch the raw outline, then
 * position it with `Rotate(rotateDegrees) * Translate(xPosition, yPosition)`
 * composed into ONE affine and applied once. The single composition is
 * load-bearing rather than incidental: the CPU backend's golden images are
 * pinned to that arithmetic, so \ref UnplacedGlyphOutline's two-step form is
 * not a drop-in substitute for it.
 *
 * Returns an empty path for `.notdef` (glyphIndex 0) or when the font has no
 * vector outline for the glyph (e.g. bitmap-only fonts) - callers handle those
 * cases (skip / bitmap path) exactly as before.
 *
 * @param textEngine Engine providing glyph outlines.
 * @param font Font handle for the run.
 * @param glyph The positioned glyph (carries position, rotation, stretch).
 * @param scale Per-run pixel-height scale (`scaleForPixelHeight(font, sizePx)`).
 * @return The placed outline path in the text element's local space.
 */
Path PlacedGlyphOutline(const TextEngine& textEngine, FontHandle font, const TextRun& run,
                        const TextGlyph& glyph, float scale, double warpTolerance);

/// The face that draws one glyph, with its outline scale and whether it has only bitmap glyphs.
struct GlyphFaceScale {
  FontHandle font;          ///< Face that draws the glyph.
  float scale = 0.0f;       ///< `scaleForPixelHeight` of \ref font at the run's used size.
  bool bitmapOnly = false;  ///< True when \ref font has no vector outlines.
};

/**
 * Resolves the face that draws \p glyph. A glyph without a fallback face reuses the run's
 * already computed values; a fallback glyph queries its own face.
 *
 * @param textEngine Engine providing font metrics.
 * @param run Run that contains \p glyph.
 * @param glyph Glyph to resolve.
 * @param runScale `scaleForPixelHeight` of the run's face at its used size.
 * @param runBitmapOnly Whether the run's face has only bitmap glyphs.
 */
GlyphFaceScale ResolveGlyphFace(const TextEngine& textEngine, const TextRun& run,
                                const TextGlyph& glyph, float runScale, bool runBitmapOnly);

/// Warp a glyph-local outline onto the perpendicular vectors of a stretched text path.
Path WarpGlyphOutline(const Path& outline, const StretchedTextPath& mapping, const TextGlyph& glyph,
                      double tolerance = Path::kLocalFlattenTolerance);

/// Derive a path-local warp tolerance from a local-to-device transform.
double TextWarpToleranceFor(const Transform2d& deviceFromLocal);

/**
 * @brief Compute the text element's bounding box for `objectBoundingBox` paint.
 *
 * Encodes tiny-skia's computation: the union over rendered glyphs of em-box
 * cells - horizontally `[xPosition, xPosition + xAdvance]`, vertically
 * `[yPosition - ascent*scale, yPosition - descent*scale]` (font v-metrics, not
 * the raw font size), per the SVG spec for text `objectBoundingBox`. A `tspan`
 * has no bbox of its own, so gradient/pattern paint on a span maps through this
 * element-level box.
 *
 * Returns an empty (default) `Box2d` when there are no rendered glyphs.
 *
 * @param textEngine Engine providing per-run scale + font v-metrics.
 * @param runs Positioned layout runs.
 * @return The text bounding box in the element's local space.
 */
Box2d ComputeTextBounds(const TextEngine& textEngine, const std::vector<TextRun>& runs);

/**
 * @brief Drop the glyphs of runs this draw is not responsible for painting.
 *
 * Two reasons take a run out of a draw:
 *
 * 1. `visibility: hidden` and `visibility: collapse` suppress painting only, so such a span is
 *    still laid out, still advances the pen for the spans that follow it, and still contributes to
 *    the element's object bounding box.
 * 2. A span whose `clip-path`, `mask`, or `filter` gives it its own rendering instance is painted
 *    by that instance, inside that effect's layer, and not by the text root's draw. `effectOwner`
 *    names the instance responsible, and \p spanEffectOwner is the one doing the drawing.
 *
 * Backends call this on their local copy of the layout runs after \ref ComputeTextBounds, so the
 * element-level bounding box that `objectBoundingBox` paint maps through stays the same for every
 * draw of the element.
 *
 * Runs are index-aligned with \p text.spans; a run past the end of the span list is left alone.
 *
 * @param text The computed text component supplying per-span visibility and ownership.
 * @param spanEffectOwner The span entity this draw paints, or `entt::null` for the text root's own
 *   draw.
 * @param runs Layout runs to filter in place.
 */
void ClearUnpaintedSpanGlyphs(const components::ComputedTextComponent& text,
                              entt::entity spanEffectOwner, std::vector<TextRun>& runs);

/// Emit bounded decoration paths while preserving each originating declaration.
/// @param run Laid-out glyph run.
/// @param origin Declaration whose geometry is emitted.
/// @param originCount Number of declarations affecting the span.
/// @param fontSize Used run size for direct-caller fallback metrics.
/// @param engine Font metrics provider.
/// @param deviceScale Device scale in the decoration thickness direction.
/// @param remainingWork Shared per-draw geometry work allowance.
/// @param emit Paint callback for each generated path.
void EmitTextDecorationPaths(const TextRun& run, const components::TextDecorationOrigin& origin,
                             std::size_t originCount, float fontSize, const TextEngine& engine,
                             double deviceScale, std::size_t& remainingWork,
                             const std::function<void(const Path&)>& emit);

}  // namespace donner::svg
