#include "donner/editor/CommandQueue.h"

#include <unordered_map>
#include <utility>

#include "donner/base/EcsRegistry.h"

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

  // Coalesce SetTransform by element identity. We walk forward and remember
  // the index each element's most recent SetTransform landed at; later
  // writes overwrite the earlier slot in-place. Order across distinct
  // elements is preserved. The element identity key is pulled from the
  // SVGElement handle — using the underlying entity id is safe because
  // we're only using it as an opaque key, not dereferencing it into the
  // registry.
  std::unordered_map<Entity, std::size_t> setTransformSlot;

  for (std::size_t i = startIndex; i < pending_.size(); ++i) {
    EditorCommand& cmd = pending_[i];

    if (cmd.kind == EditorCommand::Kind::SetTransform && cmd.element.has_value()) {
      const Entity key = cmd.element->entityHandle().entity();
      auto [it, inserted] = setTransformSlot.try_emplace(key, effective.size());
      if (inserted) {
        effective.push_back(std::move(cmd));
      } else {
        // Overwrite the previous SetTransform for this element in-place.
        effective[it->second] = std::move(cmd);
      }
    } else {
      // ReplaceDocument or DeleteElement: just emit. (At most one
      // ReplaceDocument survives the startIndex walk above.)
      effective.push_back(std::move(cmd));
    }
  }

  pending_.clear();
  return effective;
}

}  // namespace donner::editor
