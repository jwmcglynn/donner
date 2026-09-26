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
///   that painted each glyph), serializes each placed glyph `Path` to an SVG
///   `<path d="...">` in paint order, and builds a detached replacement `<g>`
///   group carrying one `<path>` per glyph.
/// - Paint is preserved from the text element's *computed style* (via
///   `SVGElement::getComputedStyle()`), not just its presentation attributes, so
///   every styling form survives: presentation attributes, CSS rules (class and
///   inline `style`), inherited paint, `currentColor` (resolved against the
///   element's `color`), and `url(#id)` paint-server references. The group
///   carries the text root's resolved `fill`, `fill-rule`, `fill-opacity`,
///   `stroke`, `stroke-width`, `stroke-opacity`, `opacity`, and `transform`;
///   per-`<tspan>` paint that differs from the group is applied on the
///   individual glyph `<path>`s so multi-color runs are not collapsed.
/// - `opacity` is the one property that multiplies rather than overrides: the
///   group carries the `<text>` element's own `opacity`, and each glyph
///   `<path>` carries the product of the `opacity` of the spans between it and
///   that `<text>`, so a translucent `<tspan>` stays translucent and the root's
///   opacity is applied exactly once.
///
/// Two `opacity` cases the emitted form cannot express exactly, both bounded to
/// a translucent run and measured against the text this conversion replaces:
///
/// - A run's `opacity` is written on each of its glyph `<path>`s rather than on
///   a per-run `<g opacity>`. The two differ only where glyphs of one run
///   overlap: per-path each glyph composites separately, a per-run group
///   composites the run once. Donner's text renderer folds a span's `opacity`
///   into each glyph's paint alpha, so it composites overlapping glyphs of a
///   translucent span twice as well (measured: two glyphs of one `opacity="0.5"`
///   span overlap at 0.75, matching two `<path opacity="0.5">`, where a single
///   layer would be 0.5). Per-path therefore keeps the outlines identical to the
///   text they replace. The group form is what the SVG group model describes, so
///   once a span composites as one layer this should become a per-run
///   `<g opacity>`.
/// - A run that both fills and strokes composites the two as one isolated layer
///   through the path's `opacity`, which is the spec-correct reading. The text
///   renderer instead multiplies the span's `opacity` into the fill alpha and
///   the stroke alpha separately, so its stroke-over-fill overlap is the
///   approximation, and a stroked translucent run renders slightly differently
///   there after conversion.
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

  /// Detached `<path>` elements, one per non-empty placed glyph, in paint
  /// order. Insert into `outlineGroup` in this order.
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
  std::vector<ConvertTextToOutlinesResult>
      conversions;  //!< Per-element conversion outcomes in the requested batch.
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
