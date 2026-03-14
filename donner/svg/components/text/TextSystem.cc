// @file
#include "donner/svg/components/text/TextSystem.h"

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextPathComponent.h"
#include "donner/svg/components/text/TextPositioningComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/graph/Reference.h"

namespace donner::svg::components {

namespace {

/// Count the number of Unicode codepoints in a UTF-8 string.
size_t countUtf8Codepoints(std::string_view str) {
  size_t count = 0;
  size_t i = 0;
  while (i < str.size()) {
    const auto byte = static_cast<uint8_t>(str[i]);
    if (byte < 0x80) {
      i += 1;
    } else if ((byte & 0xE0) == 0xC0) {
      i += 2;
    } else if ((byte & 0xF0) == 0xE0) {
      i += 3;
    } else if ((byte & 0xF8) == 0xF0) {
      i += 4;
    } else {
      i += 1;  // Invalid leading byte, skip.
    }
    ++count;
  }
  return count;
}

}  // namespace

void TextSystem::instantiateAllComputedComponents(Registry& registry,
                                                  std::vector<ParseError>* outWarnings) {
  auto view = registry.view<TextRootComponent, TextComponent, TextPositioningComponent>();
  for (auto entity : view) {
    auto [textComponent, positioningComponent] = view.get(entity);
    auto& computed = registry.get_or_emplace<ComputedTextComponent>(entity);

    computed.spans.clear();

    // Global character index across all spans within this <text> element.
    // Used to map root-level positioning lists to per-character positions.
    size_t globalCharIndex = 0;

    auto appendSpan = [&](EntityHandle handle, const TextComponent& text,
                          const TextPositioningComponent& pos) {
      ComputedTextComponent::TextSpan span;
      span.text = text.text;
      span.start = 0;
      span.end = static_cast<std::size_t>(text.text.size());

      // Set scalar positions for backwards compatibility (initial pen position).
      if (!pos.x.empty()) {
        span.x = pos.x[0];
      } else if (!positioningComponent.x.empty()) {
        span.x = positioningComponent.x[0];
      }
      if (!pos.y.empty()) {
        span.y = pos.y[0];
      } else if (!positioningComponent.y.empty()) {
        span.y = positioningComponent.y[0];
      }
      if (!pos.dx.empty()) {
        span.dx = pos.dx[0];
      } else if (!positioningComponent.dx.empty()) {
        span.dx = positioningComponent.dx[0];
      }
      if (!pos.dy.empty()) {
        span.dy = pos.dy[0];
      } else if (!positioningComponent.dy.empty()) {
        span.dy = positioningComponent.dy[0];
      }

      if (!pos.rotateDegrees.empty()) {
        span.rotateDegrees = pos.rotateDegrees[0];
      } else if (!positioningComponent.rotateDegrees.empty()) {
        span.rotateDegrees = positioningComponent.rotateDegrees[0];
      }

      // Populate per-character positioning lists.
      // Each character gets a value from its own element's list first, falling back to the root
      // text element's list at the global character index.
      const size_t charCount = countUtf8Codepoints(text.text);

      span.xList.resize(charCount);
      span.yList.resize(charCount);
      span.dxList.resize(charCount);
      span.dyList.resize(charCount);

      for (size_t ci = 0; ci < charCount; ++ci) {
        const size_t globalIdx = globalCharIndex + ci;

        if (ci < pos.x.size()) {
          span.xList[ci] = pos.x[ci];
        } else if (globalIdx < positioningComponent.x.size()) {
          span.xList[ci] = positioningComponent.x[globalIdx];
        }

        if (ci < pos.y.size()) {
          span.yList[ci] = pos.y[ci];
        } else if (globalIdx < positioningComponent.y.size()) {
          span.yList[ci] = positioningComponent.y[globalIdx];
        }

        if (ci < pos.dx.size()) {
          span.dxList[ci] = pos.dx[ci];
        } else if (globalIdx < positioningComponent.dx.size()) {
          span.dxList[ci] = positioningComponent.dx[globalIdx];
        }

        if (ci < pos.dy.size()) {
          span.dyList[ci] = pos.dy[ci];
        } else if (globalIdx < positioningComponent.dy.size()) {
          span.dyList[ci] = positioningComponent.dy[globalIdx];
        }
      }

      // Rotate: local values take priority; per SVG spec last value repeats in layout.
      if (!pos.rotateDegrees.empty()) {
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

      // Read baseline-shift from the element's computed style, or its parent's.
      // baseline-shift is a CSS property (not inherited), so check this entity first.
      auto* style = handle.try_get<ComputedStyleComponent>();
      if (!style || !style->properties) {
        const auto& tree = handle.get<donner::components::TreeComponent>();
        if (tree.parent() != entt::null) {
          EntityHandle parentHandle(registry, tree.parent());
          style = parentHandle.try_get<ComputedStyleComponent>();
        }
      }
      if (style && style->properties) {
        span.baselineShift = style->properties->baselineShift.getRequired();
        span.alignmentBaseline = style->properties->alignmentBaseline.getRequired();
      }

      // Check if this element or an ancestor is a textPath element.
      // Text content inside <textPath> is on child nodes, so walk up to find TextPathComponent.
      const TextPathComponent* textPath = handle.try_get<TextPathComponent>();
      if (!textPath) {
        const auto& tree = handle.get<donner::components::TreeComponent>();
        if (tree.parent() != entt::null) {
          EntityHandle parentHandle(registry, tree.parent());
          textPath = parentHandle.try_get<TextPathComponent>();
        }
      }
      if (textPath) {
        resolveTextPath(registry, *textPath, span, outWarnings);
      }

      computed.spans.push_back(span);
    };

    EntityHandle handle(registry, entity);
    donner::components::ForAllChildrenRecursive(handle, [&](EntityHandle cur) {
      if (!cur.all_of<TextComponent, TextPositioningComponent>()) {
        return;
      }

      appendSpan(cur, cur.get<TextComponent>(), cur.get<TextPositioningComponent>());
    });
  }
}

void TextSystem::resolveTextPath(Registry& registry, const TextPathComponent& textPath,
                                 ComputedTextComponent::TextSpan& span,
                                 std::vector<ParseError>* outWarnings) {
  if (textPath.href.empty()) {
    return;
  }

  // Resolve the href to find the referenced path element.
  const Reference ref(textPath.href);
  const auto resolved = ref.resolve(registry);
  if (!resolved || !resolved->handle) {
    return;
  }

  // Ensure the referenced element has a computed path.
  EntityHandle pathHandle = resolved->handle;
  auto* computedPath = pathHandle.try_get<ComputedPathComponent>();
  if (!computedPath) {
    // Try to compute it.
    ShapeSystem shapeSystem;
    computedPath = shapeSystem.createComputedPathIfShape(pathHandle, FontMetrics(), outWarnings);
  }

  if (!computedPath || computedPath->spline.empty()) {
    return;
  }

  span.pathSpline = computedPath->spline;

  // Resolve startOffset. Percentage is relative to path length.
  if (textPath.startOffset) {
    const double pathLen = computedPath->spline.pathLength();
    if (textPath.startOffset->unit == Lengthd::Unit::Percent) {
      span.pathStartOffset = textPath.startOffset->value * pathLen / 100.0;
    } else {
      // Use a default viewbox for pixel conversion (no font-relative units expected here).
      span.pathStartOffset =
          textPath.startOffset->toPixels(Boxd({0, 0}, {0, 0}), FontMetrics(), Lengthd::Extent::X);
    }
  }
}

}  // namespace donner::svg::components
