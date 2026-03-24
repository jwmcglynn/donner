// @file
#include "donner/svg/components/text/TextSystem.h"

#include <algorithm>
#include <functional>
#include <unordered_map>

#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextPathComponent.h"
#include "donner/svg/components/text/TextPositioningComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/core/Display.h"
#include "donner/svg/graph/Reference.h"

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

    // Per-entity subtree character offset: tracks how many characters have been seen
    // within each element's subtree, for cascading per-character coordinate lookups.
    std::unordered_map<Entity, size_t> entitySubtreeCharOffset;

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

      // Helper: for a given character index ci, walk up from the span's element
      // through ancestors to find the first applicable positioning value. Each
      // ancestor's list is indexed by the character's offset within that ancestor's
      // subtree (tracked in entitySubtreeCharOffset).
      auto findAncestorValue =
          [&](size_t ci, const auto& getList) -> std::optional<Lengthd> {
        // First try the element's own list (only for the first chunk).
        const auto& elemList = getList(pos);
        if (applyElementPositioning && ci < elemList.size()) {
          return elemList[ci];
        }

        // Walk up ancestors from parent to root.
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

          // The character's index within this ancestor's subtree.
          // Missing entries default to 0 (start of subtree).
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

      // Rotate: local values take priority; per SVG spec last value repeats in layout.
      // For the element's first chunk, use the element's own rotate list.
      // For continuation chunks (text after child elements), use globalCharIndex to
      // index into the ancestor's rotate list at the correct offset.
      if (applyElementPositioning && !pos.rotateDegrees.empty()) {
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

      // Update per-ancestor subtree character offsets: add charCount to every
      // ancestor so that subsequent spans know their offset within each ancestor.
      {
        Entity current = handle.entity();
        while (current != entt::null) {
          entitySubtreeCharOffset[current] += charCount;
          auto* tree = registry.try_get<donner::components::TreeComponent>(current);
          if (!tree || tree->parent() == entt::null) {
            break;
          }
          current = tree->parent();
        }
      }

      // Store a back-reference to the source entity so the renderer can look up
      // ComputedStyleComponent for fill, opacity, font-weight, clip-path, etc.
      span.sourceEntity = handle.entity();

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
      bool hidden = false;  ///< True if this span's element or an ancestor has display:none.
    };

    std::vector<PendingSpan> pendingSpans;

    std::function<void(EntityHandle, size_t, bool)> collectSpans =
        [&](EntityHandle handle, size_t depth, bool parentHidden) {
      if (!handle.all_of<TextComponent, TextPositioningComponent>()) {
        return;
      }

      // Skip nested <text> elements — per SVG spec, <text> inside <text> is invalid
      // and should not render. Only <tspan> children are processed.
      if (depth > 0 && handle.all_of<TextRootComponent>()) {
        return;
      }

      // Check if this element has display:none. Hidden state propagates to children.
      bool isHidden = parentHidden;
      if (!isHidden) {
        auto* style = handle.try_get<ComputedStyleComponent>();
        if (style && style->properties &&
            style->properties->display.getRequired() == Display::None) {
          isHidden = true;
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

    collectSpans(EntityHandle(registry, entity), 0, /*parentHidden=*/false);

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
      if (pending.hidden) {
        computed.spans.back().hidden = true;
      }
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
