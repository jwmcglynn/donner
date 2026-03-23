// @file
#include "donner/svg/components/text/TextSystem.h"

#include <algorithm>
#include <functional>

#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextPathComponent.h"
#include "donner/svg/components/text/TextPositioningComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/graph/Reference.h"
#include "donner/svg/properties/PaintServer.h"

namespace donner::svg::components {

namespace {

/// Count the number of UTF-16 code units in a UTF-8 string.
/// The SVG DOM uses UTF-16 indexing for per-character attributes (x, y, dx, dy, rotate).
/// Supplementary characters (U+10000+, encoded as 4 UTF-8 bytes) count as 2 (surrogate pair).
size_t countUtf16CodeUnits(std::string_view str) {
  size_t count = 0;
  size_t i = 0;
  while (i < str.size()) {
    const auto byte = static_cast<uint8_t>(str[i]);
    if (byte < 0x80) {
      i += 1;
      count += 1;
    } else if ((byte & 0xE0) == 0xC0) {
      i += 2;
      count += 1;
    } else if ((byte & 0xF0) == 0xE0) {
      i += 3;
      count += 1;
    } else if ((byte & 0xF8) == 0xF0) {
      i += 4;
      count += 2;  // Supplementary character = surrogate pair in UTF-16.
    } else {
      i += 1;  // Invalid leading byte, skip.
      count += 1;
    }
  }
  return count;
}

RcString collapseTextWhitespace(std::string_view rawText, bool preserveSpaces) {
  std::string collapsed;
  collapsed.reserve(rawText.size());

  char previous = '\0';
  for (char ch : rawText) {
    if (ch == '\r' || ch == '\n' || ch == '\t') {
      ch = ' ';
    }

    if (!preserveSpaces && ch == ' ' && previous == ' ') {
      continue;
    }

    collapsed.push_back(ch);
    previous = ch;
  }

  return RcString(collapsed);
}

/// Check if the given entity or any of its ancestors has xml:space="preserve".
bool hasXmlSpacePreserve(Registry& registry, Entity entity) {
  using donner::components::AttributesComponent;
  using donner::components::TreeComponent;

  Entity current = entity;
  while (current != entt::null) {
    if (auto* attrs = registry.try_get<AttributesComponent>(current)) {
      auto value = attrs->getAttribute(xml::XMLQualifiedNameRef("xml", "space"));
      if (value.has_value()) {
        return *value == "preserve";
      }
    }
    if (auto* tree = registry.try_get<TreeComponent>(current)) {
      current = tree->parent();
    } else {
      break;
    }
  }
  return false;
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

}  // namespace

void TextSystem::instantiateAllComputedComponents(Registry& registry,
                                                  std::vector<ParseError>* outWarnings) {
  auto view = registry.view<TextRootComponent, TextComponent, TextPositioningComponent>();
  for (auto entity : view) {
    const auto& positioningComponent = view.get<TextPositioningComponent>(entity);
    auto& computed = registry.get_or_emplace<ComputedTextComponent>(entity);

    computed.spans.clear();

    // Global character index across all spans within this <text> element.
    // Used to map root-level positioning lists to per-character positions.
    size_t globalCharIndex = 0;
    auto appendSpan = [&](EntityHandle handle, const RcString& spanText,
                          const TextPositioningComponent& pos, bool applyElementPositioning) {
      ComputedTextComponent::TextSpan span;
      span.text = spanText;
      span.start = 0;
      span.end = static_cast<std::size_t>(spanText.size());

      // Determine if this span starts a new text chunk (has its own explicit x or y).
      span.startsNewChunk = applyElementPositioning && (!pos.x.empty() || !pos.y.empty());

      // Populate per-character positioning lists.
      // Each character gets a value from its own element's list first, falling back to the root
      // text element's list at the global character index.
      // Ensure at least 1 entry even for empty text, so empty spans can propagate position
      // changes to subsequent spans.
      const size_t charCount = countUtf16CodeUnits(spanText);
      const size_t listSize = std::max(charCount, size_t(1));

      span.xList.resize(listSize);
      span.yList.resize(listSize);
      span.dxList.resize(listSize);
      span.dyList.resize(listSize);

      for (size_t ci = 0; ci < listSize; ++ci) {
        const size_t globalIdx = globalCharIndex + ci;

        // Only apply pos.x/y/dx/dy[ci] for the element's first chunk
        // (applyElementPositioning=true). For continuation chunks (text after child
        // elements), pos values at local index ci were already consumed by the first
        // chunk. Fall through to globalIdx indexing for the root's coordinate lists.
        if (applyElementPositioning && ci < pos.x.size()) {
          span.xList[ci] = pos.x[ci];
        } else if (globalIdx < positioningComponent.x.size()) {
          span.xList[ci] = positioningComponent.x[globalIdx];
        }

        if (applyElementPositioning && ci < pos.y.size()) {
          span.yList[ci] = pos.y[ci];
        } else if (globalIdx < positioningComponent.y.size()) {
          span.yList[ci] = positioningComponent.y[globalIdx];
        }

        if (applyElementPositioning && ci < pos.dx.size()) {
          span.dxList[ci] = pos.dx[ci];
        } else if (globalIdx < positioningComponent.dx.size()) {
          span.dxList[ci] = positioningComponent.dx[globalIdx];
        }

        if (applyElementPositioning && ci < pos.dy.size()) {
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
        span.fontWeight = style->properties->fontWeight.getRequired();

        const css::RGBA currentColor = style->properties->color.getRequired().rgba();
        const float fillOpacity = static_cast<float>(style->properties->fillOpacity.getRequired());
        const PaintServer fill = style->properties->fill.getRequired();
        if (const auto* solid = std::get_if<PaintServer::Solid>(&fill.value)) {
          span.fillColor = css::Color(solid->color.resolve(currentColor, fillOpacity));
        }
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

    struct PendingSpan {
      EntityHandle handle;
      RcString text;
      bool applyElementPositioning;
      size_t depth;
      bool preserveSpaces;
    };

    std::vector<PendingSpan> pendingSpans;

    std::function<void(EntityHandle, size_t)> collectSpans = [&](EntityHandle handle,
                                                                 size_t depth) {
      if (!handle.all_of<TextComponent, TextPositioningComponent>()) {
        return;
      }

      // Skip nested <text> elements — per SVG spec, <text> inside <text> is invalid
      // and should not render. Only <tspan> children are processed.
      if (depth > 0 && handle.all_of<TextRootComponent>()) {
        return;
      }

      const auto& text = handle.get<TextComponent>();
      const bool preserveSpaces = hasXmlSpacePreserve(registry, handle.entity());

      size_t chunkIndex = 0;
      bool emittedFirstChunk = false;
      auto emitChunk = [&](const RcString& chunk) {
        if (!emittedFirstChunk || !chunk.empty()) {
          pendingSpans.push_back(PendingSpan{handle, collapseTextWhitespace(chunk, preserveSpaces),
                                             !emittedFirstChunk, depth, preserveSpaces});
          emittedFirstChunk = true;
        }
      };

      if (text.textChunks.empty()) {
        emitChunk(text.text);
      } else {
        emitChunk(text.textChunks[0]);
      }

      donner::components::ForAllChildren(handle, [&](EntityHandle child) {
        collectSpans(child, depth + 1);

        ++chunkIndex;
        if (chunkIndex < text.textChunks.size()) {
          emitChunk(text.textChunks[chunkIndex]);
        }
      });
    };

    collectSpans(EntityHandle(registry, entity), 0);

    if (!pendingSpans.empty()) {
      // With xml:space="preserve", skip all inter-span space normalization.
      if (!pendingSpans.front().preserveSpaces) {
        removeLeadingSpace(pendingSpans.front().text);
      }

      std::optional<size_t> lastNonEmpty;
      for (size_t i = 0; i + 1 < pendingSpans.size(); ++i) {
        size_t currentIndex = i;
        if (pendingSpans[currentIndex].text.empty() && lastNonEmpty.has_value()) {
          currentIndex = *lastNonEmpty;
        }

        RcString& current = pendingSpans[currentIndex].text;
        RcString& next = pendingSpans[i + 1].text;
        if (next.empty()) {
          continue;
        }

        if (!pendingSpans[currentIndex].preserveSpaces && !pendingSpans[i + 1].preserveSpaces) {
          const bool currentEndsWithSpace =
              !current.empty() && current.data()[current.size() - 1] == ' ';
          const bool nextStartsWithSpace = next.data()[0] == ' ';

          if (pendingSpans[currentIndex].depth < pendingSpans[i + 1].depth) {
            if (nextStartsWithSpace) {
              removeLeadingSpace(next);
            }
          } else if (currentEndsWithSpace && nextStartsWithSpace) {
            removeTrailingSpace(current);
          }
        }

        if (!current.empty()) {
          lastNonEmpty = currentIndex;
        }
      }

      if (!pendingSpans.back().preserveSpaces) {
        removeTrailingSpace(pendingSpans.back().text);
      }
    }

    for (const PendingSpan& pending : pendingSpans) {
      const auto& pos = pending.handle.get<TextPositioningComponent>();
      appendSpan(pending.handle, pending.text, pos, pending.applyElementPositioning);
    }
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
