#include "donner/editor/SourceStructuralMove.h"

#include <algorithm>
#include <array>
#include <string>

#include "donner/base/xml/XMLNode.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/LockState.h"
#include "donner/editor/SourceSelection.h"

namespace donner::editor {
namespace {

std::uint64_t SourceHash(std::string_view source) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const char byte : source) {
    hash ^= static_cast<std::uint8_t>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string_view CanonicalEditorSource(std::string_view source) {
  if (!source.empty() && source.back() == '\n') {
    source.remove_suffix(1);
  }
  return source;
}

bool IsElementOrDescendant(const svg::SVGElement& ancestor, const svg::SVGElement& element) {
  for (std::optional<svg::SVGElement> current = element; current.has_value();
       current = current->parentElement()) {
    if (*current == ancestor) {
      return true;
    }
  }
  return false;
}

bool IsMoveContainer(const svg::SVGElement& element) {
  const std::string tagName(element.tagName().name);
  constexpr std::array<std::string_view, 10> kContainerTags = {
      "svg", "g", "defs", "symbol", "a", "marker", "mask", "pattern", "clipPath", "switch",
  };
  return std::find(kContainerTags.begin(), kContainerTags.end(), tagName) != kContainerTags.end();
}

std::optional<std::size_t> ResolvedOffset(std::string_view source, const FileOffset& offset) {
  const FileOffset resolved = offset.resolveOffset(source);
  return resolved.offset.has_value()
             ? std::optional<std::size_t>(std::min(*resolved.offset, source.size()))
             : std::nullopt;
}

std::optional<std::size_t> InsertionOffset(std::string_view source, const svg::SVGElement& parent,
                                           const std::optional<svg::SVGElement>& referenceElement) {
  if (referenceElement.has_value()) {
    const std::optional<SourceByteRange> range = ElementSourceByteRange(*referenceElement, source);
    return range.has_value() ? std::optional<std::size_t>(range->start) : std::nullopt;
  }

  const std::optional<xml::XMLNode> parentNode = xml::XMLNode::TryCast(parent.entityHandle());
  if (!parentNode.has_value()) {
    return std::nullopt;
  }
  const std::optional<SourceRange> closingTag = parentNode->getClosingTagLocation();
  return closingTag.has_value() ? ResolvedOffset(source, closingTag->start) : std::nullopt;
}

}  // namespace

SourceStructuralMoveEvaluation BuildSourceStructuralMovePlan(
    EditorApp& app, std::string_view source, svg::SVGElement element, svg::SVGElement parent,
    std::optional<svg::SVGElement> referenceElement) {
  if (!app.hasDocument()) {
    return {.status = SourceStructuralMoveStatus::DocumentUnavailable};
  }
  svg::SVGDocument& document = app.document().document();
  if (!document.hasSourceStore() || CanonicalEditorSource(document.source()) != source) {
    return {.status = SourceStructuralMoveStatus::StaleRevision};
  }

  const svg::SVGElement root = document.svgElement();
  if (element == root) {
    return {.status = SourceStructuralMoveStatus::RootElement};
  }
  if (!IsElementOrDescendant(root, element) || !IsElementOrDescendant(root, parent) ||
      !IsMoveContainer(parent)) {
    return {.status = SourceStructuralMoveStatus::InvalidParent};
  }
  if (IsLocked(element) || IsLocked(parent)) {
    return {.status = SourceStructuralMoveStatus::Locked};
  }
  if (IsElementOrDescendant(element, parent)) {
    return {.status = SourceStructuralMoveStatus::Cycle};
  }

  if (referenceElement.has_value()) {
    if (*referenceElement == element || referenceElement->parentElement() != parent) {
      return {.status = SourceStructuralMoveStatus::InvalidReference};
    }
  }
  if (element.parentElement() == parent && element.nextSibling() == referenceElement) {
    return {.status = SourceStructuralMoveStatus::NoChange};
  }

  const std::optional<SourceByteRange> elementRange = ElementSourceByteRange(element, source);
  const std::optional<std::size_t> insertionOffset =
      InsertionOffset(source, parent, referenceElement);
  if (!elementRange.has_value() || !insertionOffset.has_value()) {
    return {.status = SourceStructuralMoveStatus::NoSourceRange};
  }

  return {
      .status = SourceStructuralMoveStatus::Ready,
      .plan =
          SourceStructuralMovePlan{
              .element = element,
              .parent = parent,
              .referenceElement = referenceElement,
              .elementRange = *elementRange,
              .insertionOffset = *insertionOffset,
              .expectedDocumentGeneration = app.document().documentGeneration(),
              .expectedFrameVersion = app.document().currentFrameVersion(),
              .expectedSourceHash = SourceHash(source),
          },
  };
}

SourceStructuralMoveStatus CommitSourceStructuralMove(EditorApp& app,
                                                      const SourceStructuralMovePlan& plan,
                                                      std::string_view source) {
  if (!app.hasDocument()) {
    return SourceStructuralMoveStatus::DocumentUnavailable;
  }
  if (app.document().documentGeneration() != plan.expectedDocumentGeneration ||
      app.document().currentFrameVersion() != plan.expectedFrameVersion ||
      SourceHash(source) != plan.expectedSourceHash ||
      CanonicalEditorSource(app.document().document().source()) != source) {
    return SourceStructuralMoveStatus::StaleRevision;
  }
  return app.moveElementBefore(plan.element, plan.parent, plan.referenceElement,
                               "Move element from source")
             ? SourceStructuralMoveStatus::Ready
             : SourceStructuralMoveStatus::Rejected;
}

std::string_view SourceStructuralMoveStatusMessage(SourceStructuralMoveStatus status) {
  switch (status) {
    case SourceStructuralMoveStatus::Ready: return "Move element here";
    case SourceStructuralMoveStatus::DocumentUnavailable: return "No editable document";
    case SourceStructuralMoveStatus::StaleRevision: return "Source changed; start the drag again";
    case SourceStructuralMoveStatus::RootElement: return "The document root cannot be moved";
    case SourceStructuralMoveStatus::Locked: return "Locked elements cannot be moved";
    case SourceStructuralMoveStatus::InvalidParent:
      return "This element cannot contain SVG children";
    case SourceStructuralMoveStatus::InvalidReference: return "Invalid insertion target";
    case SourceStructuralMoveStatus::Cycle: return "An element cannot move inside itself";
    case SourceStructuralMoveStatus::NoSourceRange: return "Source location is unavailable";
    case SourceStructuralMoveStatus::NoChange: return "The element is already here";
    case SourceStructuralMoveStatus::Rejected: return "The document rejected this move";
  }
  return "Move unavailable";
}

}  // namespace donner::editor
