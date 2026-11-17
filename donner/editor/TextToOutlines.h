#pragma once
/// @file
///
/// Pure text-to-outline conversion helper, decoupled from ImGui and from the
/// editor shell so it can be unit-tested headlessly (see
/// `donner/editor/tests/TextToOutlines_tests.cc`).
///
/// This implements the structural core of "Convert Text to Outlines":
///
/// - `convertTextToOutlines` resolves the computed glyph outlines for a single
///   selected `<text>` element using Donner's renderer-facing text geometry
///   (`SVGTextElement::convertToOutlineGlyphs()`, which drives the same
///   `TextEngine` geometry the renderer consumes and retains the source element
///   that painted each glyph, the text run that placed it, and where its text
///   chunks start), serializes the placed glyph `Path`s to SVG `<path d="...">`
///   elements in paint order, and builds a detached replacement `<g>` group to
///   hold them.
/// - Each run is painted as the text renderer paints it. A run without a visible
///   stroke becomes one fill-only `<path>` per glyph. A run with one becomes one
///   fill-only `<path>` per glyph, left out when the fill is `none`, and one
///   stroke-only `<path>` per text chunk holding every glyph of the chunk, in the
///   run's `paint-order`: the renderer fills a run glyph by glyph and strokes each
///   chunk as one shape, so where glyph strokes meet, the stroke is covered once.
/// - Paint is preserved from the text element's *computed style* (via
///   `SVGElement::getComputedStyle()`), not just its presentation attributes, so
///   every styling form survives: presentation attributes, CSS rules (class and
///   inline `style`), inherited paint, `currentColor` (resolved against the
///   element's `color`), and `url(#id)` paint-server references. The group
///   carries the text root's resolved `fill`, `fill-rule`, `fill-opacity`,
///   `stroke`, `stroke-width`, `stroke-opacity`, `stroke-dasharray`,
///   `stroke-dashoffset`, `stroke-linecap`, `stroke-linejoin`,
///   `stroke-miterlimit`, `opacity`, and `transform`, every paint attribute
///   written even at its default so none is inherited from the `<text>`'s
///   ancestors; per-`<tspan>` paint that differs from the group is applied on
///   the individual `<path>`s so multi-color runs are not collapsed. A
///   fill-only `<path>` carries only fill overrides and a stroke-only `<path>`
///   only stroke overrides. Font-relative stroke widths, dash lengths and dash
///   offsets are written in user units, resolved against the font size of the
///   element whose run they paint (for an inherited value, the descendant, not
///   the element that set it), as the renderer resolves a span's stroke lengths.
/// - The text's `vector-effect: non-scaling-stroke`, which is not inherited, is
///   written on each stroke-only `<path>`. The outlines then match the text under
///   a uniform scale, rotation or reflection only: under any other transform the
///   renderer strokes text in local space at the geometric-mean width, while a
///   path with `vector-effect` is stroked exactly in host space (a
///   pattern-painted path stroke stays in local space too).
/// - The group carries the `<text>` element's own `opacity`, which composites
///   the outlines as one layer, as it did the text. The `opacity` of the spans
///   between a run and the `<text>` is not written as `opacity`: it is folded into
///   the `fill-opacity` of the run's fill-only `<path>`s and the `stroke-opacity`
///   of its stroke-only `<path>`s, as the text renderer folds a span's `opacity`
///   into each glyph's fill alpha and each chunk's stroke alpha. Where two glyphs
///   of a translucent span overlap they composite twice, on the text and on the
///   outlines alike (two `opacity="0.5"` glyphs overlap at 0.75), and the fill
///   and stroke of a translucent span composite separately.
///
/// The conversion is DOM-first: it builds unattached DOM elements and never
/// changes the authored tree or source. Detached elements share document storage.
/// The caller applies them as ordinary structural DOM
/// edits - insert the group before the `<text>` (preserving paint order),
/// insert each path into the group, delete the `<text>` - through the
/// editor's mutation seam (`EditorCommand::InsertElementCommand` /
/// `DeleteElementCommand`), so source reflection emits deltas and entity
/// identity elsewhere in the document survives.
///
/// Error handling: if glyph-outline extraction fails or yields empty
/// outlines, the function fails as a whole (`ok == false`, no elements
/// created in the document tree) and the caller must not mutate the document.

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

/// Result of preparing a text-to-outline conversion.
struct ConvertTextToOutlinesResult {
  /// Whether the conversion can be applied. When false, `error` describes why
  /// and no elements were built - the caller must not mutate the document.
  bool ok = false;

  /// Human-readable failure reason for the user when `ok` is false. Names the
  /// blocking element where possible.
  std::string error;

  /// The detached replacement `<g>` group (style attributes applied, no
  /// children yet). Populated when `ok`. Insert it before the original
  /// `<text>`, then insert each of `outlinePaths` into it in order.
  std::optional<svg::SVGElement> outlineGroup;

  /// Detached `<path>` elements in paint order: a fill-only `<path>` per
  /// non-empty placed glyph of a run without a visible stroke; for a run with
  /// one, a fill-only `<path>` per glyph (none when the fill is `none`) and a
  /// stroke-only `<path>` per text chunk, ordered by the run's `paint-order`.
  /// Insert into `outlineGroup` in this order.
  std::vector<svg::SVGElement> outlinePaths;

  /// The `id` of the replacement outline `<g>` group. Always populated when
  /// `ok`; the caller selects this after applying the edits.
  std::string outlineGroupId;
};

/// All-or-error result for a set of text-to-outline conversions.
struct ConvertTextsToOutlinesResult {
  bool ok = false;    //!< Whether every selected element was converted.
  std::string error;  //!< Failure reason; populated when ok is false.

  /// Detached conversions in input order. Empty on any failure.
  std::vector<ConvertTextToOutlinesResult> conversions;
};

/// Preflight every target before constructing any detached replacements, under one write guard.
/// The authored tree and source remain unchanged on both success and failure.
/// @param document Document containing all text elements.
/// @param textElements Text targets, in the desired conversion order.
[[nodiscard]] ConvertTextsToOutlinesResult convertTextsToOutlines(
    svg::SVGDocument& document, std::span<const svg::SVGElement> textElements);

/// Prepare a text-to-outline conversion of \p textElement within \p document.
///
/// Builds detached replacement elements; does not attach them or mutate the
/// document tree. See file comment for outline extraction, grouping, and style
/// preservation semantics.
///
/// @param document Document containing \p textElement.
/// @param textElement The `<text>` element to convert. Must be a `<text>`
///   element belonging to \p document.
[[nodiscard]] ConvertTextToOutlinesResult convertTextToOutlines(svg::SVGDocument& document,
                                                                const svg::SVGElement& textElement);

}  // namespace donner::editor
