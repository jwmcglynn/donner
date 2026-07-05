#include "donner/editor/CommandQueue.h"

#include <map>
#include <unordered_map>
#include <utility>

#include "donner/base/EcsRegistry.h"

namespace donner::editor {

CommandQueue::FlushResult CommandQueue::flush() {
  if (pending_.empty()) {
    return {};
  }

  // Find the latest structural-replace and drop everything queued before it.
  // Commands queued after the latest structural-replace survive coalescing
  // against each other, but anything before it is logically wiped out.
  // ReplaceDocument, CutShapes, and PasteShapes are all "swap the whole
  // document" commands - they invalidate any element handles that earlier
  // commands hold, so prior commands must be discarded.
  auto isStructuralReplace = [](EditorCommand::Kind kind) {
    return kind == EditorCommand::Kind::ReplaceDocument || kind == EditorCommand::Kind::CutShapes ||
           kind == EditorCommand::Kind::PasteShapes;
  };

  std::size_t startIndex = 0;
  bool hadReplaceDocument = false;
  bool allReplaceDocumentPreserveUndo = true;
  for (std::size_t i = 0; i < pending_.size(); ++i) {
    if (isStructuralReplace(pending_[i].kind)) {
      hadReplaceDocument = true;
      allReplaceDocumentPreserveUndo =
          allReplaceDocumentPreserveUndo && pending_[i].preserveUndoOnReparse;
      startIndex = i;
    }
  }

  FlushResult result;
  result.hadReplaceDocument = hadReplaceDocument;
  result.preserveUndoOnReparse = hadReplaceDocument && allReplaceDocumentPreserveUndo;
  result.effectiveCommands.reserve(pending_.size() - startIndex);

  // Coalesce SetTransform by element identity. We walk forward and remember
  // the index each element's most recent SetTransform landed at; later
  // writes overwrite the earlier slot in-place. Order across distinct
  // elements is preserved. The element identity key is pulled from the
  // SVGElement handle - using the underlying entity id is safe because
  // we're only using it as an opaque key, not dereferencing it into the
  // registry.
  std::unordered_map<Entity, std::size_t> setTransformSlot;

  // Coalesce SetAttribute by (element, attributeName). Successive writes
  // to the same attribute on the same element collapse to the most recent
  // value, just like SetTransform. The key is the (entity, attrName) pair.
  std::map<std::pair<Entity, std::string>, std::size_t> setAttributeSlot;

  for (std::size_t i = startIndex; i < pending_.size(); ++i) {
    EditorCommand& cmd = pending_[i];

    if (cmd.kind == EditorCommand::Kind::SetTransform && cmd.element.has_value()) {
      const Entity key = cmd.element->unsafeEntityHandle().entity();
      auto [it, inserted] = setTransformSlot.try_emplace(key, result.effectiveCommands.size());
      if (inserted) {
        result.effectiveCommands.push_back(std::move(cmd));
      } else {
        result.effectiveCommands[it->second] = std::move(cmd);
      }
    } else if (cmd.kind == EditorCommand::Kind::SetAttribute && cmd.element.has_value()) {
      const auto key =
          std::make_pair(cmd.element->unsafeEntityHandle().entity(), cmd.attributeName);
      auto [it, inserted] = setAttributeSlot.try_emplace(key, result.effectiveCommands.size());
      if (inserted) {
        result.effectiveCommands.push_back(std::move(cmd));
      } else {
        result.effectiveCommands[it->second] = std::move(cmd);
      }
    } else {
      // ReplaceDocument, InsertElement, or DeleteElement: just emit. (At most one
      // ReplaceDocument survives the startIndex walk above.)
      result.effectiveCommands.push_back(std::move(cmd));
    }
  }

  pending_.clear();
  return result;
}

}  // namespace donner::editor
