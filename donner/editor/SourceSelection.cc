#include "donner/editor/SourceSelection.h"

#include "donner/base/FileOffset.h"
#include "donner/base/xml/XMLNode.h"

namespace donner::editor {

namespace {

Coordinates FileOffsetToEditorCoordinates(const TextEditor& textEditor, const FileOffset& offset) {
  if (offset.offset.has_value()) {
    return textEditor.getCoordinatesAtByteOffset(*offset.offset);
  }

  if (offset.lineInfo.has_value()) {
    return Coordinates(static_cast<int>(offset.lineInfo->line) - 1,
                       static_cast<int>(offset.lineInfo->offsetOnLine));
  }

  return Coordinates(0, 0);
}

}  // namespace

bool HighlightElementSource(TextEditor& textEditor, const svg::SVGElement& element) {
  auto xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return false;
  }

  auto range = xmlNode->getNodeLocation();
  if (!range.has_value()) {
    return false;
  }

  textEditor.selectAndFocus(FileOffsetToEditorCoordinates(textEditor, range->start),
                            FileOffsetToEditorCoordinates(textEditor, range->end));
  return true;
}

}  // namespace donner::editor
