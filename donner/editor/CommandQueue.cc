#include "donner/editor/CommandQueue.h"

#include <algorithm>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "donner/base/EcsRegistry.h"

namespace donner::editor {

namespace {

bool IsStructuralReplace(EditorCommand::Kind kind) {
  return kind == EditorCommand::Kind::ReplaceDocument || kind == EditorCommand::Kind::PasteShapes;
}

/// Coalescing key for attribute commands: the element plus the attribute's qualified name.
using AttributeSlotKey = std::tuple<Entity, std::string, std::string>;

AttributeSlotKey AttributeSlotKeyFor(const EditorCommand& command) {
  return AttributeSlotKey(command.element->unsafeEntityHandle().entity(),
                          command.attributeNamespacePrefix, command.attributeName);
}

void StopAttributeCoalescingAtRemoval(const EditorCommand& command,
                                      std::map<AttributeSlotKey, std::size_t>& attributeSlots) {
  if (command.kind == EditorCommand::Kind::RemoveAttribute && command.element.has_value()) {
    attributeSlots.erase(AttributeSlotKeyFor(command));
  }
}

}  // namespace

std::optional<std::string> CommandQueue::pendingStyleAttribute(
    const svg::SVGElement& element) const {
  for (auto it = pending_.rbegin(); it != pending_.rend(); ++it) {
    if (IsStructuralReplace(it->kind)) {
      break;
    }
    if (!it->element.has_value() || *it->element != element || it->attributeName != "style" ||
        !it->attributeNamespacePrefix.empty()) {
      continue;
    }
    if (it->kind == EditorCommand::Kind::SetAttribute) {
      return it->attributeValue;
    }
    if (it->kind == EditorCommand::Kind::RemoveAttribute) {
      return std::string();
    }
  }
  return std::nullopt;
}

void CommandQueue::discardStaleSourceRecoveries(
    std::optional<EditorCommand::SourceRecoveryState> current) {
  std::erase_if(pending_, [&](const EditorCommand& command) {
    return command.sourceRecoveryState && command.sourceRecoveryState != current;
  });
}

CommandQueue::FlushResult CommandQueue::flush() {
  if (pending_.empty()) {
    return {};
  }

  // Find the latest structural-replace and drop everything queued before it.
  // Commands queued after the latest structural-replace survive coalescing
  // against each other, but anything before it is logically wiped out.
  // ReplaceDocument and PasteShapes are both "swap the whole
  // document" commands - they invalidate any element handles that earlier
  // commands hold, so prior commands must be discarded.
  std::size_t startIndex = 0;
  bool hadReplaceDocument = false;
  bool allReplaceDocumentPreserveUndo = true;
  for (std::size_t i = 0; i < pending_.size(); ++i) {
    if (IsStructuralReplace(pending_[i].kind)) {
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

  // Coalesce SetAttribute by (element, qualified attribute name). Successive
  // writes to the same attribute on the same element collapse to the most
  // recent value, just like SetTransform. `href` and `xlink:href` are distinct.
  std::map<AttributeSlotKey, std::size_t> setAttributeSlot;

  for (std::size_t i = startIndex; i < pending_.size(); ++i) {
    EditorCommand& cmd = pending_[i];
    StopAttributeCoalescingAtRemoval(cmd, setAttributeSlot);

    if (cmd.kind == EditorCommand::Kind::SetTransform && cmd.element.has_value()) {
      const Entity key = cmd.element->unsafeEntityHandle().entity();
      auto [it, inserted] = setTransformSlot.try_emplace(key, result.effectiveCommands.size());
      if (inserted) {
        result.effectiveCommands.push_back(std::move(cmd));
      } else {
        result.effectiveCommands[it->second] = std::move(cmd);
      }
    } else if (cmd.kind == EditorCommand::Kind::SetAttribute && cmd.element.has_value()) {
      auto [it, inserted] =
          setAttributeSlot.try_emplace(AttributeSlotKeyFor(cmd), result.effectiveCommands.size());
      if (inserted) {
        result.effectiveCommands.push_back(std::move(cmd));
      } else {
        result.effectiveCommands[it->second] = std::move(cmd);
      }
    } else {
      // Structural replacements, insertions, and deletions: just emit. (At
      // most one ReplaceDocument survives the startIndex walk above.)
      result.effectiveCommands.push_back(std::move(cmd));
    }
  }

  pending_.clear();
  return result;
}

}  // namespace donner::editor
