#include "donner/editor/backend_lib/SourceSync.h"

#include "donner/editor/backend_lib/ChangeClassifier.h"
#include "donner/editor/backend_lib/EditorCommand.h"

namespace donner::editor {

void QueueSourceWritebackReparse(EditorApp& app, std::string_view newSource,
                                 std::string_view previousSourcePrePatch,
                                 std::string* previousSourceText,
                                 std::optional<std::string>* lastWritebackSourceText) {
  *previousSourceText = std::string(newSource);
  *lastWritebackSourceText = *previousSourceText;

  // Incremental path: try to classify the patch as a targeted attribute
  // edit on a single element. If the writeback only touched one
  // `transform="..."` value (the drag-end case), this produces a
  // `SetAttributeCommand` that updates the attribute on the live DOM
  // without rebuilding the entity space. The compositor's cached layer
  // bitmaps + segments survive, so the user doesn't see a freeze when
  // they let go of a drag on a complex document.
  if (app.structuredEditingEnabled() && app.hasDocument()) {
    auto classified =
        classifyTextChange(app.document().document(), previousSourcePrePatch, newSource);
    if (classified.command.has_value()) {
      app.applyMutation(std::move(*classified.command));
      return;
    }
  }

  // Fall-through: the writeback's diff range spans more than one
  // attribute value, or structured editing is off. Full reparse is
  // still correct — with `Option B` landed on the compositor
  // (`setDocumentMaybeStructural`), the reparse now takes the
  // structural-remap path when the tree shape is unchanged, so the
  // fallback still avoids the full-reset cost in the common case.
  app.applyMutation(
      EditorCommand::ReplaceDocumentCommand(*previousSourceText, /*preserveUndoOnReparse=*/true));
}

DispatchSourceTextChangeResult DispatchSourceTextChange(
    EditorApp& app, std::string_view newSource, std::string* previousSourceText,
    std::optional<std::string>* lastWritebackSourceText) {
  if (lastWritebackSourceText->has_value() && newSource == **lastWritebackSourceText) {
    *previousSourceText = std::string(newSource);
    lastWritebackSourceText->reset();
    return DispatchSourceTextChangeResult{
        .dispatchedMutation = false,
        .skippedSelfWriteback = true,
    };
  }

  if (newSource == *previousSourceText) {
    return {};
  }

  bool handled = false;
  if (app.structuredEditingEnabled() && app.hasDocument()) {
    auto classified = classifyTextChange(app.document().document(), *previousSourceText, newSource);
    if (classified.command.has_value()) {
      app.applyMutation(std::move(*classified.command));
      handled = true;
    }
  }

  if (!handled) {
    app.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(newSource)));
  }

  *previousSourceText = std::string(newSource);
  lastWritebackSourceText->reset();
  return DispatchSourceTextChangeResult{
      .dispatchedMutation = true,
      .skippedSelfWriteback = false,
  };
}

}  // namespace donner::editor
