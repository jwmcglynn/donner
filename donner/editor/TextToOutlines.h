#pragma once
/// @file
///
/// Pure text-to-outline conversion helper, decoupled from ImGui and from the
/// editor shell so it can be unit-tested headlessly (see
/// `donner/editor/tests/TextToOutlines_tests.cc`).
///
/// This implements the structural core of "Convert Text to Outlines" from
/// design doc 0047 (§ "Convert Text to Outlines" and § "Error Handling"):
///
/// - `convertTextToOutlines` resolves the computed glyph outlines for a single
///   selected `<text>` element using Donner's renderer-facing text geometry
///   (`SVGTextElement::convertToPath()`, which drives the same
///   `TextEngine::computedGlyphPaths()` the renderer consumes), serializes each
///   placed glyph `Path` to an SVG `<path d="...">` in paint order, and splices
///   a replacement `<g>` group into the document source in place of the original
///   `<text>` node's source range. Visual style (`fill`, `fill-rule`, `opacity`,
///   `stroke`, `stroke-width`, `transform`) is carried onto the group so the
///   outlined geometry lands exactly where the text rendered.
///
/// The conversion is destructive and undoable: live-text-after-conversion is an
/// explicit non-goal. The function never mutates its inputs — it returns the
/// merged full-document source for the editor to reparse through
/// `EditorCommand::Kind::ConvertTextToOutlines`, exactly like the shape
/// clipboard wires Cut/Paste through `Kind::CutShapes` / `Kind::PasteShapes`.
///
/// Per § "Error Handling": if glyph-outline extraction fails or yields empty
/// outlines, or the element's source range cannot be located, the function
/// fails as a whole (`ok == false`, empty `mergedSource`) and the caller must
/// not mutate the document.

#include <string>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

/// Result of preparing a text-to-outline conversion.
struct ConvertTextToOutlinesResult {
  /// Whether the conversion can be applied. When false, `error` describes why
  /// and `mergedSource` is empty — the caller must not mutate the document.
  bool ok = false;

  /// Human-readable failure reason for the user when `ok` is false. Names the
  /// blocking element where possible (per design doc § "Error Handling").
  std::string error;

  /// Full merged SVG document source to reparse when `ok` is true.
  std::string mergedSource;

  /// The `id` of the replacement outline `<g>` group. Always populated when
  /// `ok`; the caller selects this after the reparse.
  std::string outlineGroupId;
};

/// Prepare a text-to-outline conversion of \p textElement within \p document.
///
/// Does not mutate \p document; it computes the merged source string the editor
/// should reparse. See file comment for outline extraction, grouping, and style
/// preservation semantics.
///
/// @param document Document containing \p textElement.
/// @param textElement The `<text>` element to convert. Must be a `<text>`
///   element belonging to \p document.
[[nodiscard]] ConvertTextToOutlinesResult convertTextToOutlines(const svg::SVGDocument& document,
                                                                const svg::SVGElement& textElement);

}  // namespace donner::editor
