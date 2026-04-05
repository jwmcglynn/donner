// @file
#include "donner/svg/components/text/TextSystem.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <unordered_map>

#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/PathComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextPositioningComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/core/Display.h"
#include "donner/svg/graph/Reference.h"
#include "donner/svg/parser/PathParser.h"

namespace donner::svg::components {

namespace {

size_t countUtf16CodeUnits(std::string_view utf8) {
  size_t count = 0;
  for (unsigned char c : utf8) {
    if ((c & 0xC0) != 0x80) {
      count++;
      if ((c & 0xF8) == 0xF0) {
        count++;
      }
    }
  }
  return count;
}

bool hasXmlSpacePreserve(Registry& registry, Entity entity) {
  while (entity != entt::null) {
    if (const auto* attrs = registry.try_get<donner::components::AttributesComponent>(entity)) {
      if (auto v =
              attrs->getAttribute(xml::XMLQualifiedNameRef(RcString("xml"), RcString("space")));
          v.has_value()) {
        return v.value() == "preserve";
      }
    }
    const auto* tree = registry.try_get<donner::components::TreeComponent>(entity);
    if (!tree) {
      break;
    }
    entity = tree->parent();
  }
  return false;
}

RcString collapseTextWhitespace(std::string_view text, bool preserveSpaces) {
  if (preserveSpaces) {
    // xml:space="preserve": convert newlines/tabs to spaces but keep all spaces (no collapse).
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
      if (c == '\t' || c == '\n' || c == '\r') {
        out.push_back(' ');
      } else {
        out.push_back(c);
      }
    }
    return RcString(out);
  }

  std::string out;
  out.reserve(text.size());
  bool prevWasSpace = false;
  for (char c : text) {
    if (c == '\t' || c == '\n' || c == '\r' || c == ' ') {
      if (!prevWasSpace) {
        out.push_back(' ');
        prevWasSpace = true;
      }
    } else {
      out.push_back(c);
      prevWasSpace = false;
    }
  }
  return RcString(out);
}

void removeLeadingSpace(RcString& text) {
  if (!text.empty() && text.data()[0] == ' ') {
    text = RcString(text.data() + 1, text.size() - 1);
  }
}

void removeTrailingSpace(RcString& text) {
  if (!text.empty() && text.data()[text.size() - 1] == ' ') {
    text = RcString(text.data(), text.size() - 1);
  }
}

void resolveTextPath(Registry& registry, const TextPathComponent& textPath,
                     ComputedTextComponent::TextSpan& span, std::vector<ParseError>* outWarnings) {
  if (textPath.href.empty()) {
    return;
  }

  const Reference ref(textPath.href);
  const auto resolved = ref.resolve(registry);
  if (!resolved || !resolved->handle) {
    return;
  }

  // TODO(jwm): Resolve dependency cycle with ShapeSystem so that we don't need to re-parse the path
  // data here.
  auto* computedPath = resolved->handle.try_get<ComputedPathComponent>();
  ComputedPathComponent parsedPath;
  if (!computedPath) {
    if (const auto* pathComponent = resolved->handle.try_get<PathComponent>()) {
      if (pathComponent->splineOverride.has_value()) {
        parsedPath.spline = *pathComponent->splineOverride;
        computedPath = &parsedPath;
      } else if (const auto actualD = pathComponent->d.get();
                 actualD.has_value() && !actualD->empty()) {
        auto maybePath = parser::PathParser::Parse(actualD.value());
        if (maybePath.hasResult()) {
          parsedPath.spline = std::move(maybePath.result());
          computedPath = &parsedPath;
        }
      }
    }
  }

  if (!computedPath || computedPath->spline.empty()) {
    return;
  }

  // Apply the referenced path element's local transform to the path geometry.
  // Per SVG §10.12.2, textPath uses the path in the referenced element's user coordinate space.
  const auto* localTransform =
      registry.try_get<ComputedLocalTransformComponent>(resolved->handle.entity());
  if (localTransform && !localTransform->entityFromParent.isIdentity()) {
    const Transformd& parentFromEntity = localTransform->entityFromParent;
    const auto& srcPoints = computedPath->spline.points();
    const auto& srcCommands = computedPath->spline.commands();
    PathSpline transformed;
    for (const auto& cmd : srcCommands) {
      switch (cmd.type) {
        case PathSpline::CommandType::MoveTo:
          transformed.moveTo(parentFromEntity.transformPosition(srcPoints[cmd.pointIndex]));
          break;
        case PathSpline::CommandType::LineTo:
          transformed.lineTo(parentFromEntity.transformPosition(srcPoints[cmd.pointIndex]));
          break;
        case PathSpline::CommandType::CurveTo:
          transformed.curveTo(parentFromEntity.transformPosition(srcPoints[cmd.pointIndex]),
                              parentFromEntity.transformPosition(srcPoints[cmd.pointIndex + 1]),
                              parentFromEntity.transformPosition(srcPoints[cmd.pointIndex + 2]));
          break;
        case PathSpline::CommandType::ClosePath: transformed.closePath(); break;
      }
    }
    span.pathSpline = std::move(transformed);
  } else {
    span.pathSpline = computedPath->spline;
  }
  if (textPath.startOffset) {
    const double pathLen = span.pathSpline->pathLength();
    if (textPath.startOffset->unit == Lengthd::Unit::Percent) {
      span.pathStartOffset = textPath.startOffset->value * pathLen / 100.0;
    } else {
      span.pathStartOffset =
          textPath.startOffset->toPixels(Boxd({0, 0}, {0, 0}), FontMetrics(), Lengthd::Extent::X);
    }
  }
}

/// Find the textPath entity that applies to the given entity.
/// Sets \p outInvalidNesting to true if the entity is inside a nested (invalid) textPath
/// whose content should be hidden.
Entity findApplicableTextPathEntity(Registry& registry, Entity entity, bool& outInvalidNesting) {
  outInvalidNesting = false;
  Entity current = entity;
  while (current != entt::null) {
    if (registry.any_of<TextPathComponent>(current)) {
      const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
      if (tree && tree->parent() != entt::null &&
          registry.any_of<TextRootComponent>(tree->parent())) {
        return current;
      }
      // Found a textPath but it's not a direct child of <text> — invalid nesting.
      // Content inside should be hidden per SVG spec.
      outInvalidNesting = true;
      return entt::null;
    }

    const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
    if (!tree) {
      break;
    }
    current = tree->parent();
  }

  return entt::null;
}

}  // namespace

void TextSystem::instantiateAllComputedComponents(Registry& registry,
                                                  std::vector<ParseError>* outWarnings) {
  auto view = registry.view<TextRootComponent, TextComponent, TextPositioningComponent>();
  for (auto entity : view) {
    instantiateComputedComponent(EntityHandle(registry, entity), outWarnings);
  }
}

void TextSystem::instantiateComputedComponent(EntityHandle rootHandle,
                                              std::vector<ParseError>* outWarnings) {
  Registry& registry = *rootHandle.registry();
  UTILS_RELEASE_ASSERT(
      (rootHandle.all_of<TextRootComponent, TextComponent, TextPositioningComponent>()));

  const auto& positioningComponent = rootHandle.get<TextPositioningComponent>();
  auto& computed = registry.get_or_emplace<ComputedTextComponent>(rootHandle.entity());
  computed.spans.clear();

  size_t globalCharIndex = 0;
  std::unordered_map<Entity, size_t> entitySubtreeCharOffset;

  auto appendSpan = [&](EntityHandle handle, const RcString& spanText,
                        const TextPositioningComponent& pos, bool applyElementPositioning) {
    ComputedTextComponent::TextSpan span;
    span.text = spanText;
    span.start = 0;
    span.end = static_cast<std::size_t>(spanText.size());
    // Per SVG spec, <textPath> does not support x/y/dx/dy/rotate attributes.
    // Only apply element positioning for non-textPath elements.
    const bool isTextPath = handle.all_of<TextPathComponent>();
    const bool applyPositioning = applyElementPositioning && !isTextPath;
    span.startsNewChunk = applyPositioning && (!pos.x.empty() || !pos.y.empty());

    const size_t charCount = countUtf16CodeUnits(spanText);
    const size_t listSize = std::max(charCount, size_t(1));

    span.xList.resize(listSize);
    span.yList.resize(listSize);
    span.dxList.resize(listSize);
    span.dyList.resize(listSize);

    auto findAncestorValue = [&](size_t ci, const auto& getList) -> std::optional<Lengthd> {
      const auto& elemList = getList(pos);
      if (applyPositioning && ci < elemList.size()) {
        return elemList[ci];
      }

      Entity current = handle.entity();
      while (current != entt::null) {
        auto* tree = registry.try_get<donner::components::TreeComponent>(current);
        if (!tree || tree->parent() == entt::null) {
          break;
        }
        current = tree->parent();

        auto* ancestorPos = registry.try_get<TextPositioningComponent>(current);
        if (!ancestorPos) {
          continue;
        }
        const auto& ancestorList = getList(*ancestorPos);
        if (ancestorList.empty()) {
          continue;
        }

        size_t ancestorIdx = entitySubtreeCharOffset[current] + ci;
        if (ancestorIdx < ancestorList.size()) {
          return ancestorList[ancestorIdx];
        }
      }
      return std::nullopt;
    };

    auto getX = [](const TextPositioningComponent& p) -> const auto& { return p.x; };
    auto getY = [](const TextPositioningComponent& p) -> const auto& { return p.y; };
    auto getDx = [](const TextPositioningComponent& p) -> const auto& { return p.dx; };
    auto getDy = [](const TextPositioningComponent& p) -> const auto& { return p.dy; };

    for (size_t ci = 0; ci < listSize; ++ci) {
      span.xList[ci] = findAncestorValue(ci, getX);
      span.yList[ci] = findAncestorValue(ci, getY);
      span.dxList[ci] = findAncestorValue(ci, getDx);
      span.dyList[ci] = findAncestorValue(ci, getDy);
    }

    if (applyPositioning && !pos.rotateDegrees.empty()) {
      span.rotateList.assign(pos.rotateDegrees.begin(), pos.rotateDegrees.end());
    } else if (!positioningComponent.rotateDegrees.empty()) {
      for (size_t ci = 0; ci < charCount; ++ci) {
        const size_t globalIdx = globalCharIndex + ci;
        if (globalIdx < positioningComponent.rotateDegrees.size()) {
          span.rotateList.push_back(positioningComponent.rotateDegrees[globalIdx]);
        } else {
          break;
        }
      }
    }

    globalCharIndex += charCount;

    Entity current = handle.entity();
    while (current != entt::null) {
      entitySubtreeCharOffset[current] += charCount;
      auto* tree = registry.try_get<donner::components::TreeComponent>(current);
      if (!tree || tree->parent() == entt::null) {
        break;
      }
      current = tree->parent();
    }

    span.sourceEntity = handle.entity();

    bool invalidTextPathNesting = false;
    const Entity textPathEntity =
        findApplicableTextPathEntity(registry, handle.entity(), invalidTextPathNesting);
    if (textPathEntity != entt::null) {
      const auto& textPath = registry.get<TextPathComponent>(textPathEntity);
      resolveTextPath(registry, textPath, span, outWarnings);
      if (!span.pathSpline) {
        // textPath href could not be resolved — mark as failed so glyphs are hidden.
        span.textPathFailed = true;
      } else {
        span.textPathSourceEntity = textPathEntity;
      }
    } else if (invalidTextPathNesting) {
      // Content inside a nested (invalid) textPath — hide glyphs.
      span.textPathFailed = true;
    }

    computed.spans.push_back(span);
  };

  struct PendingSpan {
    EntityHandle handle;
    RcString text;
    bool applyElementPositioning;
    size_t depth;
    bool preserveSpaces;
    bool hidden = false;
  };

  std::vector<PendingSpan> pendingSpans;

  std::function<void(EntityHandle, size_t, bool)> collectSpans = [&](EntityHandle handle,
                                                                     size_t depth,
                                                                     bool parentHidden) {
    if (!handle.all_of<TextComponent, TextPositioningComponent>()) {
      return;
    }

    if (depth > 0 && handle.all_of<TextRootComponent>()) {
      return;
    }

    bool isHidden = parentHidden;
    if (!isHidden) {
      auto* style = handle.try_get<ComputedStyleComponent>();
      if (style && style->properties && style->properties->display.getRequired() == Display::None) {
        isHidden = true;
      }
    }

    // An invalid textPath (empty href, or nested inside another textPath) should hide its
    // content entirely so that whitespace collapsing treats it as absent.
    if (!isHidden) {
      const auto* textPathComp = handle.try_get<TextPathComponent>();
      if (textPathComp) {
        if (textPathComp->href.empty()) {
          isHidden = true;
        } else {
          // Check if this textPath is nested (not a direct child of <text>).
          const auto* tree = handle.try_get<donner::components::TreeComponent>();
          if (!tree || tree->parent() == entt::null ||
              !registry.any_of<TextRootComponent>(tree->parent())) {
            isHidden = true;
          }
        }
      }
    }

    const auto& text = handle.get<TextComponent>();
    const bool preserveSpaces = hasXmlSpacePreserve(registry, handle.entity());

    size_t chunkIndex = 0;
    bool emittedFirstChunk = false;
    auto emitChunk = [&](const RcString& chunk) {
      if (!emittedFirstChunk || !chunk.empty()) {
        pendingSpans.push_back(PendingSpan{handle, collapseTextWhitespace(chunk, preserveSpaces),
                                           !emittedFirstChunk, depth, preserveSpaces, isHidden});
        emittedFirstChunk = true;
      }
    };

    if (text.textChunks.empty()) {
      emitChunk(text.text);
    } else {
      emitChunk(text.textChunks[0]);
    }

    donner::components::ForAllChildren(handle, [&](EntityHandle child) {
      collectSpans(child, depth + 1, isHidden);

      ++chunkIndex;
      if (chunkIndex < text.textChunks.size()) {
        emitChunk(text.textChunks[chunkIndex]);
      }
    });
  };

  collectSpans(rootHandle, 0, false);

  if (!pendingSpans.empty()) {
    for (auto& span : pendingSpans) {
      if (!span.text.empty()) {
        if (!span.preserveSpaces) {
          removeLeadingSpace(span.text);
          if (!span.text.empty()) {
            break;
          }
        } else {
          break;
        }
      }
    }

    std::optional<size_t> lastNonEmpty;
    for (size_t i = 0; i + 1 < pendingSpans.size(); ++i) {
      if (!pendingSpans[i].text.empty()) {
        lastNonEmpty = i;
      }

      size_t currentIndex = lastNonEmpty.value_or(i);
      RcString& current = pendingSpans[currentIndex].text;
      RcString& next = pendingSpans[i + 1].text;
      if (next.empty()) {
        continue;
      }

      const bool currentEndsWithSpace =
          !current.empty() && current.data()[current.size() - 1] == ' ';
      const bool nextStartsWithSpace = next.data()[0] == ' ';
      if (currentEndsWithSpace && nextStartsWithSpace && !pendingSpans[i + 1].preserveSpaces) {
        removeLeadingSpace(next);
      }
    }

    for (auto it = pendingSpans.rbegin(); it != pendingSpans.rend(); ++it) {
      if (!it->text.empty()) {
        if (!it->preserveSpaces) {
          removeTrailingSpace(it->text);
          if (!it->text.empty()) {
            break;
          }
        } else {
          break;
        }
      }
    }
  }

  for (const PendingSpan& pending : pendingSpans) {
    const auto& pos = pending.handle.get<TextPositioningComponent>();
    if (pending.hidden) {
      ComputedTextComponent::TextSpan span;
      span.text = pending.text;
      span.start = 0;
      span.end = static_cast<std::size_t>(pending.text.size());
      span.sourceEntity = pending.handle.entity();
      span.hidden = true;
      computed.spans.push_back(span);
    } else {
      appendSpan(pending.handle, pending.text, pos, pending.applyElementPositioning);
    }
  }
}

}  // namespace donner::svg::components
