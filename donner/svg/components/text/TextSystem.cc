// @file
#include "donner/svg/components/text/TextSystem.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "donner/base/Utf8.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/ConditionalProcessingComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/animation/AnimationBaseValue.h"
#include "donner/svg/components/animation/AnimationValueSampler.h"
#include "donner/svg/components/layout/LayoutSystem.h"
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

size_t countCodepoints(std::string_view utf8) {
  size_t count = 0;
  while (!utf8.empty()) {
    const auto [codepoint, length] = Utf8::NextCodepoint(utf8);
    utf8.remove_prefix(static_cast<size_t>(length));
    ++count;
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

std::string NormalizeSegmentBreaks(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool previousCarriageReturn = false;
  for (char c : text) {
    if (c != '\n' || !previousCarriageReturn) {
      out.push_back(c == '\r' ? '\n' : c);
    }
    previousCarriageReturn = c == '\r';
  }
  return out;
}

RcString CollapseSpaces(std::string_view text, bool preserveBreaks) {
  std::string out;
  out.reserve(text.size());
  bool previousSpace = false;
  for (char c : text) {
    if (c == '\n' && preserveBreaks) {
      if (!out.empty() && out.back() == ' ') {
        out.pop_back();
      }
      out.push_back(c);
      previousSpace = true;
    } else if (c == '\t' || c == '\n' || c == ' ') {
      if (!previousSpace) {
        out.push_back(' ');
      }
      previousSpace = true;
    } else {
      out.push_back(c);
      previousSpace = false;
    }
  }
  return RcString(out);
}

RcString collapseTextWhitespace(std::string_view text, WhiteSpace mode, bool legacyPreserve) {
  if (legacyPreserve) {
    std::string out(text);
    for (char& c : out) {
      if (c == '\t' || c == '\n' || c == '\r') {
        c = ' ';
      }
    }
    return RcString(out);
  }
  const std::string normalized = NormalizeSegmentBreaks(text);
  return PreservesSpaces(mode) ? RcString(normalized)
                               : CollapseSpaces(normalized, PreservesSegmentBreaks(mode));
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

/// Returns true when the element declares somewhere for its glyphs to be placed, either inline
/// geometry from `path` or a reference through `href`. An element declaring neither renders
/// nothing.
bool hasTextPathGeometrySource(const TextPathComponent& textPath) {
  return (textPath.inlinePath && !textPath.inlinePath->empty()) || !textPath.href.empty();
}

/// Resolves `startOffset` against the span's already-resolved path, where a percentage is a
/// fraction of that path's total length.
void applyStartOffset(Registry& registry, Entity textPathEntity, const TextPathComponent& textPath,
                      ComputedTextComponent::TextSpan& span) {
  const auto recipe = ContextualLengthFor(registry, textPathEntity, "startOffset");
  auto* budget = registry.ctx().find<AnimationResourceBudget>();
  if (recipe && budget && budget->reserveContextualWork(recipe->evaluationWork)) {
    const auto measured = span.pathSpline->measure(budget->remainingContextualWork());
    if (budget->reserveContextualWork(measured.measurementWorkUnits()) && measured.valid()) {
      AnimationValueContext context;
      context.viewBox = Box2d::FromXYWH(0, 0, measured.pathLength(), measured.pathLength());
      context.extent = Lengthd::Extent::X;
      const Lengthd offset =
          SampleContextualLengthRecipe(*recipe, textPath.startOffset.value_or(Lengthd()), context)
              .value_or(textPath.startOffset.value_or(Lengthd()));
      span.pathStartOffset = offset.toPixels(context.viewBox, context.fontMetrics, context.extent);
      return;
    }
  }
  if (!textPath.startOffset) {
    return;
  }

  if (textPath.startOffset->unit == Lengthd::Unit::Percent) {
    span.pathStartOffset = textPath.startOffset->value * span.pathSpline->pathLength() / 100.0;
  } else {
    span.pathStartOffset =
        textPath.startOffset->toPixels(Box2d({0, 0}, {0, 0}), FontMetrics(), Lengthd::Extent::X);
  }
}

/// Resolves the geometry a \ref xml_textPath places glyphs on, in the textPath element's own user
/// space, or an empty result when the element names no usable geometry.
std::optional<Path> resolveTextPathGeometry(Registry& registry, const TextPathComponent& textPath) {
  // The `path` attribute wins over `href` when it parsed to geometry; its coordinates are already
  // in the textPath element's user space, so no referenced-element transform applies.
  if (textPath.inlinePath && !textPath.inlinePath->empty()) {
    return *textPath.inlinePath;
  }

  if (textPath.href.empty()) {
    return std::nullopt;
  }

  const Reference ref(textPath.href);
  const auto resolved = ref.resolve(registry);
  if (!resolved || !resolved->handle) {
    return std::nullopt;
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
    return std::nullopt;
  }

  // The referenced path is treated as if it were defined in the textPath user space. Its own
  // pivoted transform applies there, while transforms on its original ancestors do not.
  const Transform2d textRootFromPath =
      LayoutSystem().getEntityFromParentTransform(resolved->handle);
  if (!textRootFromPath.isIdentity()) {
    const auto& srcPoints = computedPath->spline.points();
    const auto& srcCommands = computedPath->spline.commands();
    PathBuilder builder;
    for (const auto& cmd : srcCommands) {
      switch (cmd.verb) {
        case Path::Verb::MoveTo:
          builder.moveTo(textRootFromPath.transformPosition(srcPoints[cmd.pointIndex]));
          break;
        case Path::Verb::LineTo:
          builder.lineTo(textRootFromPath.transformPosition(srcPoints[cmd.pointIndex]));
          break;
        case Path::Verb::QuadTo:
          builder.quadTo(textRootFromPath.transformPosition(srcPoints[cmd.pointIndex]),
                         textRootFromPath.transformPosition(srcPoints[cmd.pointIndex + 1]));
          break;
        case Path::Verb::CurveTo:
          builder.curveTo(textRootFromPath.transformPosition(srcPoints[cmd.pointIndex]),
                          textRootFromPath.transformPosition(srcPoints[cmd.pointIndex + 1]),
                          textRootFromPath.transformPosition(srcPoints[cmd.pointIndex + 2]));
          break;
        case Path::Verb::ClosePath: builder.closePath(); break;
      }
    }
    return builder.build();
  }

  return computedPath->spline;
}

/// Resolves the geometry and start offset a \ref xml_textPath places glyphs on, leaving \p span
/// unchanged when no usable geometry exists.
void resolveTextPath(Registry& registry, Entity textPathEntity, const TextPathComponent& textPath,
                     ComputedTextComponent::TextSpan& span) {
  std::optional<Path> geometry = resolveTextPathGeometry(registry, textPath);
  if (!geometry) {
    return;
  }

  // `side="right"` puts glyphs on the other side of the path, which is the same as travelling the
  // path backwards: glyphs face the opposite way and `startOffset` counts from what was the end.
  span.pathSpline =
      textPath.side == TextPathSide::Right ? geometry->reversed() : std::move(*geometry);
  span.textPathMethod = textPath.method;
  applyStartOffset(registry, textPathEntity, textPath, span);
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
      // Found a textPath but it's not a direct child of <text> - invalid nesting.
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

/// Resolves rotations from the nearest authored list, indexed across that element's subtree.
void resolveInheritedRotations(EntityHandle handle, size_t charCount,
                               const std::unordered_map<Entity, size_t>& subtreeCharOffsets,
                               SmallVector<double, 1>& rotations) {
  Registry& registry = *handle.registry();
  Entity current = handle.entity();
  while (current != entt::null) {
    const auto* positioning = registry.try_get<TextPositioningComponent>(current);
    if (positioning && !registry.any_of<TextPathComponent>(current) &&
        !positioning->rotateDegrees.empty()) {
      const auto offset = subtreeCharOffsets.find(current);
      const auto& values = positioning->rotateDegrees;
      const size_t startIndex =
          std::min(offset != subtreeCharOffsets.end() ? offset->second : 0, values.size() - 1);
      const size_t valueCount = std::min(charCount, values.size() - startIndex);
      for (size_t ci = 0; ci < valueCount; ++ci) {
        rotations.push_back(values[startIndex + ci]);
      }
      return;
    }

    if (registry.any_of<TextRootComponent>(current)) {
      return;
    }

    const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
    if (!tree) {
      return;
    }
    current = tree->parent();
  }
}

}  // namespace

namespace {

bool CanCollectTextSpan(EntityHandle handle) {
  return handle.all_of<TextComponent, TextPositioningComponent>() &&
         HasSvgNamespace(*handle.registry(), handle.entity());
}

struct PendingTextSpan {
  EntityHandle handle;
  RcString text;
  bool applyElementPositioning;
  size_t depth;
  bool preserveSpaces;
  WhiteSpace whiteSpace;
  bool hidden = false;
};

bool IsHiddenByStyle(EntityHandle handle) {
  const auto* style = handle.try_get<ComputedStyleComponent>();
  return style && style->properties && style->properties->display.get().value() == Display::None;
}

bool IsHiddenByConditionalProcessing(EntityHandle handle,
                                     const std::vector<RcString>& userLanguages) {
  const auto* conditional = handle.try_get<ConditionalProcessingComponent>();
  return conditional && !EvaluateConditionalProcessing(*conditional, userLanguages);
}

bool IsHiddenTextPath(EntityHandle handle) {
  const auto* textPath = handle.try_get<TextPathComponent>();
  if (!textPath) {
    return false;
  }

  if (!hasTextPathGeometrySource(*textPath)) {
    return true;
  }

  const auto* tree = handle.try_get<donner::components::TreeComponent>();
  return !tree || tree->parent() == entt::null ||
         !handle.registry()->any_of<TextRootComponent>(tree->parent());
}

bool IsTextHidden(EntityHandle handle, bool parentHidden,
                  const std::vector<RcString>& userLanguages) {
  if (parentHidden || IsHiddenByStyle(handle)) {
    return true;
  }

  if (IsHiddenByConditionalProcessing(handle, userLanguages)) {
    return true;
  }

  return IsHiddenTextPath(handle);
}

void TrimLeadingPendingSpace(std::vector<PendingTextSpan>& pendingSpans) {
  for (PendingTextSpan& span : pendingSpans) {
    if (span.text.empty()) {
      continue;
    }

    if (span.preserveSpaces) {
      break;
    }

    removeLeadingSpace(span.text);
    if (!span.text.empty()) {
      break;
    }
  }
}

void TrimTrailingPendingSpace(std::vector<PendingTextSpan>& pendingSpans) {
  for (auto it = pendingSpans.rbegin(); it != pendingSpans.rend(); ++it) {
    if (it->text.empty()) {
      continue;
    }

    if (it->preserveSpaces) {
      break;
    }

    removeTrailingSpace(it->text);
    if (!it->text.empty()) {
      break;
    }
  }
}

void UpdateLastVisibleSpan(const PendingTextSpan& span, size_t index,
                           std::optional<size_t>& lastNonEmpty) {
  if (span.hidden) {
    lastNonEmpty = std::nullopt;
  } else if (!span.text.empty()) {
    lastNonEmpty = index;
  }
}

bool HasCollapsibleBoundary(const PendingTextSpan& current, const PendingTextSpan& next) {
  if (next.text.empty() || next.hidden || next.preserveSpaces) {
    return false;
  }

  const bool currentEndsWithSpace =
      !current.text.empty() && current.text.data()[current.text.size() - 1] == ' ';
  const bool nextStartsWithSpace = next.text.data()[0] == ' ';
  return currentEndsWithSpace && nextStartsWithSpace;
}

void CollapsePendingBoundary(PendingTextSpan& current, PendingTextSpan& next) {
  if (!HasCollapsibleBoundary(current, next)) {
    return;
  }

  // When collapsing across a textPath boundary, remove the textPath span's trailing space
  // (keeping it off the path) rather than the next span's leading space. This ensures the
  // inter-element space survives as flat text.
  if (current.handle.all_of<TextPathComponent>() &&
      current.handle.entity() != next.handle.entity()) {
    removeTrailingSpace(current.text);
  } else {
    removeLeadingSpace(next.text);
  }
}

void CollapseAdjacentPendingSpaces(std::vector<PendingTextSpan>& pendingSpans) {
  std::optional<size_t> lastNonEmpty;
  for (size_t i = 0; i + 1 < pendingSpans.size(); ++i) {
    UpdateLastVisibleSpan(pendingSpans[i], i, lastNonEmpty);
    if (!lastNonEmpty) {
      continue;
    }

    CollapsePendingBoundary(pendingSpans[*lastNonEmpty], pendingSpans[i + 1]);
  }
}

void TrimBeforeBreak(std::vector<PendingTextSpan>& spans, size_t index) {
  for (size_t j = index; j > 0;) {
    auto& previous = spans[--j];
    if (previous.hidden || previous.preserveSpaces) {
      break;
    }
    removeTrailingSpace(previous.text);
    if (!previous.text.empty()) {
      break;
    }
  }
}

void TrimAfterBreak(std::vector<PendingTextSpan>& spans, size_t index) {
  for (size_t j = index + 1; j < spans.size(); ++j) {
    auto& next = spans[j];
    if (next.hidden || next.preserveSpaces) {
      break;
    }
    removeLeadingSpace(next.text);
    if (!next.text.empty()) {
      break;
    }
  }
}

void NormalizePendingSpaces(std::vector<PendingTextSpan>& pendingSpans) {
  for (size_t i = 0; i < pendingSpans.size(); ++i) {
    const auto& span = pendingSpans[i];
    if (span.text.empty() || span.hidden) {
      continue;
    }
    if (span.text.data()[0] == '\n') {
      TrimBeforeBreak(pendingSpans, i);
    }
    if (span.text.data()[span.text.size() - 1] == '\n') {
      TrimAfterBreak(pendingSpans, i);
    }
  }
  TrimLeadingPendingSpace(pendingSpans);
  CollapseAdjacentPendingSpaces(pendingSpans);
  TrimTrailingPendingSpace(pendingSpans);
}

Entity TextParent(Registry& registry, Entity entity) {
  const auto* tree = registry.try_get<donner::components::TreeComponent>(entity);
  return tree ? tree->parent() : entt::null;
}

WhiteSpace CommonAncestorWhiteSpace(Registry& registry, Entity first, Entity second) {
  std::unordered_set<Entity> ancestors;
  for (Entity entity = first; entity != entt::null; entity = TextParent(registry, entity)) {
    ancestors.insert(entity);
  }
  for (Entity entity = second; entity != entt::null; entity = TextParent(registry, entity)) {
    if (ancestors.contains(entity)) {
      const auto* style = registry.try_get<ComputedStyleComponent>(entity);
      return style && style->properties ? style->properties->whiteSpace.getOr(WhiteSpace::Normal)
                                        : WhiteSpace::Normal;
    }
  }
  return WhiteSpace::Normal;
}

void ResolveSpanWhitespaceBoundaries(Registry& registry, ComputedTextComponent& text) {
  std::optional<size_t> previous;
  for (size_t i = 0; i < text.spans.size(); ++i) {
    auto& span = text.spans[i];
    if (span.text.empty() || span.hidden) {
      continue;
    }
    if (previous) {
      auto& before = text.spans[*previous];
      before.boundaryWhiteSpace =
          CommonAncestorWhiteSpace(registry, before.sourceEntity, span.sourceEntity);
    }
    previous = i;
  }
}

class ComputedTextBuilder {
public:
  ComputedTextBuilder(EntityHandle rootHandle, ComputedTextComponent& computed)
      : registry_(*rootHandle.registry()), rootHandle_(rootHandle), computed_(computed) {}

  void build() {
    computed_.spans.clear();
    computed_.textLengthOwnerFontSizes.clear();
    computed_.ownerSpanRanges.clear();

    collectUserLanguages();
    collectSpans(rootHandle_, 0, false);
    NormalizePendingSpaces(pendingSpans_);
    materializeSpans();
    ResolveSpanWhitespaceBoundaries(registry_, computed_);
  }

private:
  using LengthList = SmallVector<Lengthd, 1>;
  using LengthListMember = LengthList TextPositioningComponent::*;

  void collectUserLanguages() {
    userLanguages_ = {RcString("en")};
    if (registry_.ctx().contains<SVGDocumentContext>()) {
      userLanguages_ = registry_.ctx().get<SVGDocumentContext>().userLanguages;
    }
  }

  void collectSpans(EntityHandle handle, size_t depth, bool parentHidden) {
    if (!CanCollectTextSpan(handle)) {
      return;
    }

    if (depth > 0 && handle.all_of<TextRootComponent>()) {
      return;
    }

    const size_t firstSpan = pendingSpans_.size();
    recordOwnerFontSize(handle);
    const bool isHidden = IsTextHidden(handle, parentHidden, userLanguages_);
    emitTextChunks(handle, depth, isHidden);
    computed_.ownerSpanRanges[handle.entity()] = {firstSpan, pendingSpans_.size()};
  }

  void recordOwnerFontSize(EntityHandle handle) {
    const auto* style = handle.try_get<ComputedStyleComponent>();
    if (style && style->properties) {
      computed_.textLengthOwnerFontSizes.try_emplace(handle.entity(),
                                                     style->properties->fontSize.get().value());
    }
  }

  void emitTextChunks(EntityHandle handle, size_t depth, bool isHidden) {
    const auto& text = handle.get<TextComponent>();
    const auto* style = handle.try_get<ComputedStyleComponent>();
    const auto* property = style && style->properties ? &style->properties->whiteSpace : nullptr;
    const WhiteSpace mode = property ? property->getOr(WhiteSpace::Normal) : WhiteSpace::Normal;
    const bool legacyPreserve =
        (!property || !property->isSpecified()) && hasXmlSpacePreserve(registry_, handle.entity());
    const bool preserveSpaces = legacyPreserve || PreservesSpaces(mode);
    size_t chunkIndex = 0;
    bool emittedFirstChunk = false;

    auto emitChunk = [&](const RcString& chunk) {
      if (!emittedFirstChunk || !chunk.empty()) {
        pendingSpans_.push_back(
            PendingTextSpan{handle, collapseTextWhitespace(chunk, mode, legacyPreserve),
                            !emittedFirstChunk, depth, preserveSpaces, mode, isHidden});
        emittedFirstChunk = true;
      }
    };

    emitChunk(text.textChunks.empty() ? text.text : text.textChunks[0]);
    donner::components::ForAllChildren(handle, [&](EntityHandle child) {
      collectSpans(child, depth + 1, isHidden);
      ++chunkIndex;
      if (chunkIndex < text.textChunks.size()) {
        emitChunk(text.textChunks[chunkIndex]);
      }
    });
  }

  std::optional<Lengthd> findAncestorValue(EntityHandle handle, size_t charIndex,
                                           LengthListMember member) const {
    Entity current = handle.entity();
    while (current != entt::null) {
      const auto* positioning = registry_.try_get<TextPositioningComponent>(current);
      if (positioning && !registry_.any_of<TextPathComponent>(current)) {
        const auto& values = positioning->*member;
        const auto offset = entitySubtreeCharOffset_.find(current);
        const size_t startIndex = offset != entitySubtreeCharOffset_.end() ? offset->second : 0;
        if (startIndex < values.size() && charIndex < values.size() - startIndex) {
          return values[startIndex + charIndex];
        }
      }

      if (registry_.any_of<TextRootComponent>(current)) {
        break;
      }

      const auto* tree = registry_.try_get<donner::components::TreeComponent>(current);
      if (!tree) {
        break;
      }
      current = tree->parent();
    }
    return std::nullopt;
  }

  void populatePositionLists(EntityHandle handle, size_t charCount,
                             ComputedTextComponent::TextSpan& span) const {
    span.xList.resize(charCount);
    span.yList.resize(charCount);
    span.dxList.resize(charCount);
    span.dyList.resize(charCount);
    for (size_t charIndex = 0; charIndex < charCount; ++charIndex) {
      span.xList[charIndex] = findAncestorValue(handle, charIndex, &TextPositioningComponent::x);
      span.yList[charIndex] = findAncestorValue(handle, charIndex, &TextPositioningComponent::y);
      span.dxList[charIndex] = findAncestorValue(handle, charIndex, &TextPositioningComponent::dx);
      span.dyList[charIndex] = findAncestorValue(handle, charIndex, &TextPositioningComponent::dy);
    }
  }

  void populateRotations(EntityHandle handle, const TextPositioningComponent& positioning,
                         bool applyPositioning, size_t charCount,
                         ComputedTextComponent::TextSpan& span) const {
    if (applyPositioning && !positioning.rotateDegrees.empty()) {
      span.rotateList.assign(positioning.rotateDegrees.begin(), positioning.rotateDegrees.end());
      return;
    }

    resolveInheritedRotations(handle, charCount, entitySubtreeCharOffset_, span.rotateList);
  }

  void advanceSubtreeOffsets(EntityHandle handle, size_t charCount) {
    Entity current = handle.entity();
    while (current != entt::null) {
      entitySubtreeCharOffset_[current] += charCount;
      const auto* tree = registry_.try_get<donner::components::TreeComponent>(current);
      if (!tree || tree->parent() == entt::null) {
        break;
      }
      current = tree->parent();
    }
  }

  void populateTextPath(EntityHandle handle, ComputedTextComponent::TextSpan& span) {
    bool invalidTextPathNesting = false;
    const Entity textPathEntity =
        findApplicableTextPathEntity(registry_, handle.entity(), invalidTextPathNesting);
    if (textPathEntity != entt::null) {
      const auto& textPath = registry_.get<TextPathComponent>(textPathEntity);
      resolveTextPath(registry_, textPathEntity, textPath, span);
      if (!span.pathSpline) {
        // textPath href could not be resolved - mark as failed so glyphs are hidden.
        span.textPathFailed = true;
      } else {
        span.textPathSourceEntity = textPathEntity;
      }
    } else if (invalidTextPathNesting) {
      // Content inside a nested (invalid) textPath - hide glyphs.
      span.textPathFailed = true;
    }
  }

  void appendVisibleSpan(const PendingTextSpan& pending) {
    ComputedTextComponent::TextSpan span;
    span.text = pending.text;
    span.whiteSpace = pending.whiteSpace;
    span.start = 0;
    span.end = static_cast<std::size_t>(pending.text.size());

    const bool isTextPath = pending.handle.all_of<TextPathComponent>();
    const bool applyPositioning = pending.applyElementPositioning && !isTextPath;
    const size_t charCount = countCodepoints(pending.text);
    populatePositionLists(pending.handle, charCount, span);
    span.startsNewChunk = !isTextPath && !span.xList.empty() &&
                          (span.xList.front().has_value() || span.yList.front().has_value());

    const auto& positioning = pending.handle.get<TextPositioningComponent>();
    populateRotations(pending.handle, positioning, applyPositioning, charCount, span);
    advanceSubtreeOffsets(pending.handle, charCount);

    span.sourceEntity = pending.handle.entity();
    populateTextPath(pending.handle, span);
    computed_.spans.push_back(std::move(span));
  }

  void appendHiddenSpan(const PendingTextSpan& pending) {
    ComputedTextComponent::TextSpan span;
    span.text = pending.text;
    span.start = 0;
    span.end = static_cast<std::size_t>(pending.text.size());
    span.sourceEntity = pending.handle.entity();
    span.hidden = true;
    computed_.spans.push_back(std::move(span));
  }

  void materializeSpans() {
    for (const PendingTextSpan& pending : pendingSpans_) {
      if (pending.hidden) {
        appendHiddenSpan(pending);
      } else {
        appendVisibleSpan(pending);
      }
    }
  }

  Registry& registry_;
  EntityHandle rootHandle_;
  ComputedTextComponent& computed_;
  std::unordered_map<Entity, size_t> entitySubtreeCharOffset_;
  std::vector<PendingTextSpan> pendingSpans_;
  std::vector<RcString> userLanguages_;
};

}  // namespace

void TextSystem::instantiateAllComputedComponents(Registry& registry,
                                                  ParseWarningSink& warningSink) {
  auto view = registry.view<TextRootComponent, TextComponent, TextPositioningComponent>();
  for (auto entity : view) {
    instantiateComputedComponent(EntityHandle(registry, entity), warningSink);
  }
}

void TextSystem::instantiateComputedComponent(EntityHandle rootHandle,
                                              ParseWarningSink& warningSink) {
  Registry& registry = *rootHandle.registry();
  UTILS_RELEASE_ASSERT(
      (rootHandle.all_of<TextRootComponent, TextComponent, TextPositioningComponent>()));

  auto& computed = registry.get_or_emplace<ComputedTextComponent>(rootHandle.entity());
  ComputedTextBuilder(rootHandle, computed).build();
}

}  // namespace donner::svg::components
