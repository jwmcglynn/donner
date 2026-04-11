#include "donner/editor/CommandQueue.h"

#include <unordered_map>
#include <utility>

namespace donner::editor {

std::vector<EditorCommand> CommandQueue::flush() {
  if (pending_.empty()) {
    return {};
  }

  // Find the latest ReplaceDocument and drop everything queued before it.
  // Commands queued after the latest ReplaceDocument survive coalescing
  // against each other, but anything before it is logically wiped out.
  std::size_t startIndex = 0;
  for (std::size_t i = 0; i < pending_.size(); ++i) {
    if (pending_[i].kind == EditorCommand::Kind::ReplaceDocument) {
      startIndex = i;
    }
  }

  std::vector<EditorCommand> effective;
  effective.reserve(pending_.size() - startIndex);

  // Coalesce SetTransform by entity. We walk forward and remember the index
  // each entity's most recent SetTransform landed at; later writes overwrite
  // the earlier slot in-place. Order across distinct entities is preserved.
  std::unordered_map<Entity, std::size_t> setTransformSlot;

  for (std::size_t i = startIndex; i < pending_.size(); ++i) {
    EditorCommand& cmd = pending_[i];

    if (cmd.kind == EditorCommand::Kind::SetTransform) {
      auto [it, inserted] = setTransformSlot.try_emplace(cmd.entity, effective.size());
      if (inserted) {
        effective.push_back(std::move(cmd));
      } else {
        // Overwrite the previous SetTransform for this entity in-place.
        effective[it->second] = std::move(cmd);
      }
    } else {
      // ReplaceDocument: just emit. (At most one survives the startIndex
      // walk above, which by construction is the first command we visit.)
      effective.push_back(std::move(cmd));
    }
  }

  pending_.clear();
  return effective;
}

}  // namespace donner::editor
