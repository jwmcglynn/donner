#pragma once
/// @file

#include <cstddef>
#include <optional>
#include <string_view>

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
 * Finds the deepest SVG element at the text editor cursor.
 *
 * @param document Source-backed SVG document to inspect.
 * @param textEditor Source editor containing the cursor and source text.
 * @return Deepest SVG element at the cursor, or \c std::nullopt if none is found.
 */
std::optional<svg::SVGElement> FindElementAtSourceCursor(const svg::SVGDocument& document,
                                                         const TextEditor& textEditor);

}  // namespace donner::editor
