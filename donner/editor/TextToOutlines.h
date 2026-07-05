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
///   (`SVGTextElement::convertToPath()`, which drives the same
///   `TextEngine::computedGlyphPaths()` the renderer consumes), serializes each
///   placed glyph `Path` to an SVG `<path d="...">` in paint order, and builds
///   a detached replacement `<g>` group carrying one `<path>` per glyph. Visual
///   style (`fill`, `fill-rule`, `opacity`, `stroke`, `stroke-width`,
///   `transform`) is carried onto the group so the outlined geometry lands
///   exactly where the text rendered.
///
/// The conversion is DOM-first: it builds unattached DOM elements and never
/// mutates the document. The caller applies it as ordinary structural DOM
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
