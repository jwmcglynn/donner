/// @file

#include "donner/editor/TextEditor.h"

namespace donner::editor {

void TextEditor::setLanguageDefinition(const LanguageDefinition& langDef) {
  core_.setLanguageDefinition(langDef);
}

}  // namespace donner::editor
