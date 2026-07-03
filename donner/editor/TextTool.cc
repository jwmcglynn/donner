#include "donner/editor/TextTool.h"

#include <string>
#include <vector>

#include "donner/base/FormatNumber.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGTextElement.h"

namespace donner::editor {

namespace {

/// Returns the element the new text should be inserted into: the selected
/// element when it is a compatible container (a `<g>` or the root `<svg>`),
/// otherwise the document's root `<svg>`.
svg::SVGElement ResolveInsertParent(EditorApp& editor) {
  const std::vector<svg::SVGElement>& selection = editor.selectedElements();
  if (selection.size() == 1u) {
    const svg::ElementType type = selection.front().type();
    if (type == svg::ElementType::G || type == svg::ElementType::SVG) {
      return selection.front();
    }
  }
  return editor.document().document().svgElement();
}

}  // namespace

void TextTool::onMouseDown(EditorApp& editor, const Vector2d& documentPoint,
                           MouseModifiers modifiers) {
  (void)modifiers;
  if (!editor.document().hasDocument()) {
    cancel();
    return;
  }

  svg::SVGDocument& document = editor.document().document();
  svg::SVGElement parent = ResolveInsertParent(editor);

  svg::SVGTextElement text = svg::SVGTextElement::Create(document);
  text.setAttribute("x", donner::detail::FormatNumberForSVG(documentPoint.x));
  text.setAttribute("y", donner::detail::FormatNumberForSVG(documentPoint.y));
  text.setAttribute("font-family", std::string(kDefaultFontFamily));
  text.setAttribute("font-size", std::string(kDefaultFontSize));
  text.setAttribute("fill", std::string(kDefaultFill));

  previousSelection_ = editor.selectedElements();
  content_ = std::string(kDefaultContent);
  insertedParent_ = parent;
  insertedText_ = text;

  editor.applyMutation(EditorCommand::InsertTextCommand(parent, text, content_));
  editor.setSelection(text);
}

void TextTool::onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) {
  (void)editor;
  (void)documentPoint;
  (void)buttonHeld;
}

void TextTool::onMouseUp(EditorApp& editor, const Vector2d& documentPoint) {
  (void)editor;
  (void)documentPoint;
}

void TextTool::cancel() {
  insertedText_.reset();
  insertedParent_.reset();
  previousSelection_.clear();
  content_.clear();
}

void TextTool::undoInsert(EditorApp& editor) {
  if (!insertedText_.has_value()) {
    return;
  }

  editor.applyMutation(EditorCommand::DeleteElementCommand(*insertedText_));
  if (previousSelection_.empty()) {
    editor.clearSelection();
  } else {
    editor.setSelection(previousSelection_);
  }
}

void TextTool::redoInsert(EditorApp& editor) {
  if (!insertedText_.has_value() || !insertedParent_.has_value()) {
    return;
  }

  editor.applyMutation(
      EditorCommand::InsertTextCommand(*insertedParent_, *insertedText_, content_));
  editor.setSelection(*insertedText_);
}

}  // namespace donner::editor
