#include "donner/editor/AsyncSVGDocument.h"

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {

AsyncSVGDocument::AsyncSVGDocument() = default;

void AsyncSVGDocument::setDocument(svg::SVGDocument document) {
  document_ = std::move(document);
  queue_.clear();
  frameVersion_.fetch_add(1, std::memory_order_release);
}

bool AsyncSVGDocument::flushFrame() {
  if (queue_.empty()) {
    return false;
  }

  const auto effective = queue_.flush();
  if (effective.empty()) {
    return false;
  }

  for (const auto& cmd : effective) {
    applyOne(cmd);
  }

  frameVersion_.fetch_add(1, std::memory_order_release);
  return true;
}

bool AsyncSVGDocument::loadFromString(std::string_view svgBytes) {
  ParseWarningSink sink;
  auto result = svg::parser::SVGParser::ParseSVG(svgBytes, sink);
  if (result.hasError()) {
    return false;
  }
  setDocument(std::move(result.result()));
  return true;
}

void AsyncSVGDocument::applyOne(const EditorCommand& command) {
  switch (command.kind) {
    case EditorCommand::Kind::SetTransform: {
      if (!command.element.has_value()) {
        return;
      }
      // The element handle was captured by a public API (hitTest, tree
      // traversal, or querySelector) so we just cast to the graphics-
      // element interface and call the public transform setter — no
      // direct ECS access.
      auto graphics = command.element->cast<svg::SVGGraphicsElement>();
      graphics.setTransform(command.transform);
      break;
    }

    case EditorCommand::Kind::ReplaceDocument: {
      // ReplaceDocument bypasses applyOne's mid-flush context: it always
      // produces a fresh document via the parser, so we re-enter the
      // setDocument path which clears the queue (already drained) and bumps
      // the version (which `flushFrame` will bump again, harmlessly).
      (void)loadFromString(command.bytes);
      break;
    }

    case EditorCommand::Kind::DeleteElement: {
      if (!command.element.has_value()) {
        return;
      }
      // Detach via the public `SVGElement::remove()` method. The ECS
      // entity stays in the registry (orphaned, not garbage-collected)
      // so any in-flight references — stale selection, undo snapshots —
      // remain valid but invisible to the renderer because the tree
      // walk stops at the detach point. We copy to a local because
      // `remove()` is non-const and `command` is `const&`.
      svg::SVGElement element = *command.element;
      element.remove();
      break;
    }
  }
}

}  // namespace donner::editor
