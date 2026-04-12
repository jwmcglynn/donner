#pragma once
/// @file
///
/// Builds a \ref TextPatch that updates an SVG element's attribute in the
/// source text without a full re-parse. This is the bridge between a canvas
/// tool mutation (e.g. a drag that changes `transform`) and the source pane.
///
/// The writeback path finds the attribute's byte span via
/// `XMLNode::getAttributeLocation`, escapes the new value, and returns a
/// `TextPatch` splice. If the attribute doesn't exist in the source yet,
/// the patch inserts `name="value"` before the element's closing `>`.
///
/// See `docs/design_docs/structured_text_editing.md` M3.

#include <optional>
#include <string_view>

#include "donner/editor/TextPatch.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

/**
 * Build a \ref TextPatch that sets the given attribute to the new value
 * in the source text.
 *
 * @param source The current source text (from the text editor buffer).
 * @param element The target element.
 * @param attrName The attribute name (e.g. "transform", "fill").
 * @param newValue The new attribute value (unescaped — will be XML-escaped
 *   internally).
 * @return A `TextPatch` if the writeback can be performed, or `std::nullopt`
 *   if the element has no source location, the attribute value can't be
 *   escaped (contains NUL or surrogates), or the source offsets are stale.
 */
std::optional<TextPatch> buildAttributeWriteback(std::string_view source,
                                                  const svg::SVGElement& element,
                                                  std::string_view attrName,
                                                  std::string_view newValue);

}  // namespace donner::editor
