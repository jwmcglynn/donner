#include "donner/svg/SVGTextContentElement.h"

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Vector2.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg {

namespace {

const TextEngine* tryGetPreparedTextEngine(EntityHandle handle) {
  Registry& registry = *handle.registry();
  auto* textEngine = registry.ctx().find<TextEngine>();
  if (!textEngine) {
    auto& fontManager = registry.ctx().contains<FontManager>()
                            ? registry.ctx().get<FontManager>()
                            : registry.ctx().emplace<FontManager>(registry);
    textEngine = &registry.ctx().emplace<TextEngine>(fontManager, registry);
  }

  ParseWarningSink warningSink;
  textEngine->prepareForElement(handle, warningSink);
  return textEngine;
}

}  // namespace

SVGTextContentElement::SVGTextContentElement(EntityHandle handle) : SVGGraphicsElement(handle) {
  handle_.emplace<components::TextComponent>();
}

void SVGTextContentElement::invalidateTextGeometry() {
  // Walk up to the text root and remove cached text layout.
  Registry& registry = *handle_.registry();
  Entity current = handle_.entity();
  while (current != entt::null) {
    if (registry.any_of<components::TextRootComponent>(current)) {
      registry.remove<components::ComputedTextGeometryComponent>(current);
      registry.get_or_emplace<components::DirtyFlagsComponent>(current).mark(
          components::DirtyFlagsComponent::TextGeometry |
          components::DirtyFlagsComponent::RenderInstance);
      return;
    }
    const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
    if (!tree) {
      break;
    }
    current = tree->parent();
  }
}

std::vector<PathSpline> SVGTextContentElement::computedGlyphPaths() const {
  if (const TextEngine* engine = tryGetPreparedTextEngine(handle_)) {
    return engine->computedGlyphPaths(handle_);
  }

  return {};
}

Boxd SVGTextContentElement::computedInkBounds() const {
  if (const TextEngine* engine = tryGetPreparedTextEngine(handle_)) {
    return engine->computedInkBounds(handle_);
  }

  return Boxd();
}

Boxd SVGTextContentElement::computedObjectBoundingBox() const {
  if (const TextEngine* engine = tryGetPreparedTextEngine(handle_)) {
    return engine->computedObjectBoundingBox(handle_);
  }

  return Boxd();
}

std::optional<Lengthd> SVGTextContentElement::textLength() const {
  return handle_.get<components::TextComponent>().textLength;
}

void SVGTextContentElement::setTextLength(std::optional<Lengthd> value) {
  handle_.get<components::TextComponent>().textLength = value;
  invalidateTextGeometry();
}

LengthAdjust SVGTextContentElement::lengthAdjust() const {
  return handle_.get<components::TextComponent>().lengthAdjust;
}

void SVGTextContentElement::setLengthAdjust(LengthAdjust value) {
  handle_.get_or_emplace<components::TextComponent>().lengthAdjust = value;
  invalidateTextGeometry();
}

long SVGTextContentElement::getNumberOfChars() const {
  if (const TextEngine* engine = tryGetPreparedTextEngine(handle_)) {
    return engine->getNumberOfChars(handle_);
  }

  return 0;
}

double SVGTextContentElement::getComputedTextLength() const {
  if (const TextEngine* engine = tryGetPreparedTextEngine(handle_)) {
    return engine->getComputedTextLength(handle_);
  }

  return 0.0;
}

double SVGTextContentElement::getSubStringLength(std::size_t charnum, std::size_t nchars) const {
  if (const TextEngine* engine = tryGetPreparedTextEngine(handle_)) {
    return engine->getSubStringLength(handle_, charnum, nchars);
  }

  return 0.0;
}

Vector2d SVGTextContentElement::getStartPositionOfChar(std::size_t charnum) const {
  if (const TextEngine* engine = tryGetPreparedTextEngine(handle_)) {
    return engine->getStartPositionOfChar(handle_, charnum);
  }

  return Vector2d();
}

Vector2d SVGTextContentElement::getEndPositionOfChar(std::size_t charnum) const {
  if (const TextEngine* engine = tryGetPreparedTextEngine(handle_)) {
    return engine->getEndPositionOfChar(handle_, charnum);
  }

  return Vector2d();
}

Boxd SVGTextContentElement::getExtentOfChar(std::size_t charnum) const {
  if (const TextEngine* engine = tryGetPreparedTextEngine(handle_)) {
    return engine->getExtentOfChar(handle_, charnum);
  }

  return Boxd();
}

double SVGTextContentElement::getRotationOfChar(std::size_t charnum) const {
  if (const TextEngine* engine = tryGetPreparedTextEngine(handle_)) {
    return engine->getRotationOfChar(handle_, charnum);
  }

  return 0.0;
}

long SVGTextContentElement::getCharNumAtPosition(const Vector2d& point) const {
  if (const TextEngine* engine = tryGetPreparedTextEngine(handle_)) {
    return engine->getCharNumAtPosition(handle_, point);
  }

  return -1;
}

void SVGTextContentElement::selectSubString(std::size_t /*charnum*/, std::size_t /*nchars*/) {
  // TODO: implement selection highlighting
}

void SVGTextContentElement::appendText(std::string_view text) {
  auto& textComponent = handle_.get<components::TextComponent>();

  if (textComponent.text.empty()) {
    textComponent.text = text;
  } else {
    textComponent.text = textComponent.text + text;
  }

  if (textComponent.textChunks.empty()) {
    textComponent.textChunks.emplace_back(text);
  } else if (textComponent.textChunks.back().empty()) {
    textComponent.textChunks.back() = RcString(text);
  } else {
    textComponent.textChunks.emplace_back(text);
  }

  invalidateTextGeometry();
}

void SVGTextContentElement::advanceTextChunk() {
  auto& textComponent = handle_.get<components::TextComponent>();
  if (textComponent.textChunks.empty()) {
    textComponent.textChunks.push_back(RcString(""));
  }
  textComponent.textChunks.push_back(RcString(""));
  invalidateTextGeometry();
}

RcString SVGTextContentElement::textContent() const {
  return handle_.get<components::TextComponent>().text;
}

}  // namespace donner::svg
