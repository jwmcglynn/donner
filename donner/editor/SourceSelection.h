#pragma once
/// @file

#include "donner/editor/TextEditor.h"
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

}  // namespace donner::editor
