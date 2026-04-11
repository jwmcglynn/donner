#include "donner/editor/AsyncSVGDocument.h"

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/components/layout/LayoutSystem.h"
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
      if (!document_.has_value()) {
        return;
      }
      EntityHandle handle(document_->registry(), command.entity);
      if (!handle) {
        return;
      }
      svg::components::LayoutSystem().setRawEntityFromParentTransform(handle, command.transform);
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
  }
}

}  // namespace donner::editor
