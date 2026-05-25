#pragma once
/// @file

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/editor/TextEditor.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

/**
 * Selects the source range for an SVG element in the text editor.
 *
 * @param textEditor Source editor to update.
 * @param element Element whose serialized XML node should be selected.
 * @return true if a source range was found and selected.
 */
bool HighlightElementSource(TextEditor& textEditor, const svg::SVGElement& element);

/**
 * Selects a raw source byte range in the text editor.
 *
 * @param textEditor Source editor to update.
 * @param byteRange Half-open byte range to select.
 * @return true if the range was valid for the current text buffer.
 */
bool HighlightSourceByteRange(TextEditor& textEditor, SourceByteRange byteRange);

/**
 * Return the serialized XML node byte range for an ECS entity.
 *
 * This is useful for resolved references where the caller has an \ref EntityHandle but not a
 * public \ref svg::SVGElement wrapper.
 *
 * @param handle Entity whose XML node source should be resolved.
 * @param source Source text corresponding to the entity's document.
 * @return Half-open byte range for the node, or \c std::nullopt if no source range exists.
 */
std::optional<SourceByteRange> EntitySourceByteRange(EntityHandle handle, std::string_view source);

/**
 * Return the serialized XML node byte range for an SVG element.
 *
 * @param element SVG element whose source node should be resolved.
 * @param source Source text corresponding to the element's document.
 * @return Half-open byte range for the node, or \c std::nullopt if no source range exists.
 */
std::optional<SourceByteRange> ElementSourceByteRange(const svg::SVGElement& element,
                                                      std::string_view source);

/**
 * Removes elements already represented by the active editor selection from source-hover candidates.
 *
 * @param hoverElements Candidate elements resolved from the source hover position.
 * @param selectedElements Active editor selection.
 * @return Hover candidates that are not already selected.
 */
std::vector<svg::SVGElement> ExcludeSelectedSourceHoverElements(
    std::vector<svg::SVGElement> hoverElements, std::span<const svg::SVGElement> selectedElements);

/**
 * Removes the document root from automatic source-hover candidates.
 *
 * The root `<svg>` source range covers the entire document, so automatic source hover should not
 * turn that into a whole-canvas overlay preview.
 *
 * @param hoverElements Candidate elements resolved from the source hover position.
 * @param document Document whose root should be excluded.
 * @return Hover candidates that are not the document root element.
 */
std::vector<svg::SVGElement> ExcludeDocumentRootSourceHoverElement(
    std::vector<svg::SVGElement> hoverElements, const svg::SVGDocument& document);

/**
 * Finds the deepest SVG element whose source range contains \p offset.
 *
 * @param document Source-backed SVG document to inspect.
 * @param source Source text corresponding to \p document.
 * @param offset Byte offset in \p source.
 * @return Deepest SVG element at \p offset, or \c std::nullopt if none is found.
 */
std::optional<svg::SVGElement> FindElementAtSourceOffset(const svg::SVGDocument& document,
                                                         std::string_view source,
                                                         std::size_t offset);

/**
 * Finds the SVG element nearest a source caret-like offset.
 *
 * Unlike \ref FindElementAtSourceOffset, this treats positions immediately after an opening or
 * closing tag as still referring to that tag's element. This is useful for cursor and hover
 * interactions where the UI position can land on the character cell after the source token.
 *
 * @param document Source-backed SVG document to inspect.
 * @param source Source text corresponding to \p document.
 * @param offset Byte offset in \p source.
 * @return Best SVG element at or immediately before \p offset, or \c std::nullopt.
 */
std::optional<svg::SVGElement> FindElementNearSourceOffset(const svg::SVGDocument& document,
                                                           std::string_view source,
                                                           std::size_t offset);

/**
 * Finds the deepest SVG element at the text editor cursor.
 *
 * @param document Source-backed SVG document to inspect.
 * @param textEditor Source editor containing the cursor and source text.
 * @return Deepest SVG element at the cursor, or \c std::nullopt if none is found.
 */
std::optional<svg::SVGElement> FindElementAtSourceCursor(const svg::SVGDocument& document,
                                                         const TextEditor& textEditor);

}  // namespace donner::editor
