#pragma once
/// @file
///
/// Builds a \ref donner::editor::TextPatch "TextPatch" that updates an SVG element's attribute in the
/// source text. This is the bridge between a canvas tool mutation
/// (e.g. a drag that changes `transform`) and the source pane.
///
/// The writeback path first tries the element's tracked source start offset.
/// If that no longer points at the same opening tag in the current source,
/// it re-finds the element via its XML tree path, then locates the attribute's
/// byte span and returns a `TextPatch` splice. If the attribute doesn't exist
/// in the source yet, the patch inserts `name="value"` before the element's
/// closing `>`.
///
/// See `docs/design_docs/structured_text_editing.md` M3.

#include <optional>
#include <string_view>
#include <vector>

#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/editor/TextPatch.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

/// Stable element locator for canvas→text writeback and selection remap.
struct AttributeWritebackPathSegment {
  std::size_t elementChildIndex = 0;
  xml::XMLQualifiedName qualifiedName;

  bool operator==(const AttributeWritebackPathSegment&) const = default;
};

/// Path to an element through element children only, from the SVG root down.
struct AttributeWritebackTarget {
  std::vector<AttributeWritebackPathSegment> elementPath;
  std::optional<RcString> elementId;

  bool operator==(const AttributeWritebackTarget&) const = default;
};

/**
 * Capture a stable path to an element that can be resolved against a later
 * version of the source text or a re-parsed document.
 *
 * @param element The element to snapshot.
 * @return A stable locator for the element, or `std::nullopt` if the element
 *   does not have an XML node backing it.
 */
std::optional<AttributeWritebackTarget> captureAttributeWritebackTarget(
    const svg::SVGElement& element);

/**
 * Resolve a previously-captured writeback target in the current SVG document.
 *
 * @param document The current document.
 * @param target The target to resolve.
 * @return The matching element, or `std::nullopt` if the tree shape changed
 *   and the original path no longer exists.
 */
std::optional<svg::SVGElement> resolveAttributeWritebackTarget(
    svg::SVGDocument& document, const AttributeWritebackTarget& target);

/**
 * Build a \ref donner::editor::TextPatch "TextPatch" that sets the given attribute to the new value
 * in the source text.
 *
 * @param source The current source text (from the text editor buffer).
 * @param element The target element.
 * @param attrName The attribute name (e.g. "transform", "fill").
 * @param newValue The new attribute value (unescaped — will be XML-escaped
 *   internally).
 * @return A `TextPatch` if the writeback can be performed, or `std::nullopt`
 *   if the element can't be resolved in the current source, the attribute
 *   value can't be escaped (contains NUL or surrogates), or the source
 *   text is too stale/malformed to patch safely.
 */
std::optional<TextPatch> buildAttributeWriteback(std::string_view source,
                                                 const svg::SVGElement& element,
                                                 std::string_view attrName,
                                                 std::string_view newValue);

/**
 * Build a \ref donner::editor::TextPatch "TextPatch" that sets the given attribute using a stable target
 * captured from an earlier frame.
 *
 * @param source The current source text (from the text editor buffer).
 * @param target Stable locator for the target element.
 * @param attrName The attribute name (e.g. "transform", "fill").
 * @param newValue The new attribute value (unescaped — will be XML-escaped
 *   internally).
 * @return A `TextPatch` if the writeback can be performed, or `std::nullopt`
 *   if the source no longer contains the targeted element or cannot be patched
 *   safely.
 */
std::optional<TextPatch> buildAttributeWriteback(std::string_view source,
                                                 const AttributeWritebackTarget& target,
                                                 std::string_view attrName,
                                                 std::string_view newValue);

/**
 * Build a \ref donner::editor::TextPatch "TextPatch" that removes the given attribute from the target
 * element in the source text.
 *
 * @param source The current source text (from the text editor buffer).
 * @param target Stable locator for the target element.
 * @param attrName The attribute name to remove (e.g. "transform").
 * @return A `TextPatch` deleting the attribute if it exists, `std::nullopt`
 *   if the source no longer contains the targeted element, the attribute is
 *   already absent, or the source cannot be patched safely.
 */
std::optional<TextPatch> buildAttributeRemoveWriteback(std::string_view source,
                                                       const AttributeWritebackTarget& target,
                                                       std::string_view attrName);

/**
 * Build a \ref donner::editor::TextPatch "TextPatch" that removes the target element from the
 * source text.
 *
 * @param source The current source text (from the text editor buffer).
 * @param target Stable locator for the target element.
 * @return A `TextPatch` deleting the full element span if it exists, or
 *   `std::nullopt` if the source no longer contains the targeted element or
 *   cannot be patched safely.
 */
std::optional<TextPatch> buildElementRemoveWriteback(std::string_view source,
                                                     const AttributeWritebackTarget& target);

}  // namespace donner::editor
