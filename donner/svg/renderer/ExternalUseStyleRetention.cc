#include "donner/svg/renderer/ExternalUseStyleRetention.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/FontResourceGraph.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/filter/FilterEffect.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/paint/StopComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/ExternalUseStyleContext.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/components/text/TextInvalidation.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg::components {
namespace {

/// Family-budget charge for a retained parent style snapshot, released on destruction.
class RetainedStyleCharge {
public:
  RetainedStyleCharge(std::shared_ptr<DocumentResourceFamilyBudget> family, std::size_t bytes)
      : family_(std::move(family)), bytes_(bytes) {}

  ~RetainedStyleCharge() { release(); }

  RetainedStyleCharge(RetainedStyleCharge&& other) noexcept
      : family_(std::move(other.family_)), bytes_(other.bytes_) {}

  RetainedStyleCharge& operator=(RetainedStyleCharge&& other) noexcept {
    if (this != &other) {
      release();
      family_ = std::move(other.family_);
      bytes_ = other.bytes_;
    }
    return *this;
  }

  RetainedStyleCharge(const RetainedStyleCharge&) = delete;
  RetainedStyleCharge& operator=(const RetainedStyleCharge&) = delete;

private:
  void release() {
    if (family_) {
      family_->release(DocumentResourceFamilyBudget::Kind::ComputedStyle, bytes_);
      family_.reset();
    }
  }

  std::shared_ptr<DocumentResourceFamilyBudget> family_;
  std::size_t bytes_;
};

/// Whether \p paint carries no reference: none or a solid color. Only such occurrence paints are
/// retained, so no retained state holds a handle into the host document.
bool IsPlainPaint(const ResolvedPaintServer& paint) {
  return !std::holds_alternative<PaintResolvedReference>(paint);
}

/// The occurrence inputs a referenced subtree's computed styles were derived from.
struct OccurrenceInputs {
  PropertyRegistry parent;
  std::optional<ComputedResourceOpacities> resourceOpacities;
  ResolvedPaintServer fill;
  ResolvedPaintServer stroke;
  RetainedStyleCharge charge;

  bool matches(const ExternalUseStyleContext& occurrence) const {
    return occurrence.parent.properties && parent == *occurrence.parent.properties &&
           resourceOpacities == occurrence.parent.resourceOpacities &&
           IsProvablySamePaint(fill, occurrence.fill) &&
           IsProvablySamePaint(stroke, occurrence.stroke);
  }
};

/// A referenced subtree that is styled for an occurrence rather than for its own document.
struct RetainedSubtree {
  Entity root;
  /// Inputs the subtree is styled for, or `std::nullopt` once a restyle may have reached it
  /// without them.
  std::optional<OccurrenceInputs> inputs;
  /// Value of \ref ExternalUseRetainedState::draws when this subtree was last drawn.
  std::uint64_t lastDrawn = 0;
};

/// The occurrence the rendering instances were last built for.
struct RenderTreeOccurrence {
  Entity root;
  ResolvedPaintServer fill;
  ResolvedPaintServer stroke;
  /// The viewport the `<use>` establishes for a referenced `<symbol>` or `<svg>`, if any.
  std::optional<Box2d> viewport;

  bool matches(const ExternalUseStyleContext& occurrence,
               const std::optional<Box2d>& occurrenceViewport) const {
    return root == occurrence.root && IsProvablySamePaint(fill, occurrence.fill) &&
           IsProvablySamePaint(stroke, occurrence.stroke) && viewport == occurrenceViewport;
  }
};

/// Stored in the referenced document's registry context.
struct ExternalUseRetainedState {
  ExternalUseRetainedState() = default;
  ExternalUseRetainedState(ExternalUseRetainedState&&) noexcept = default;
  ExternalUseRetainedState& operator=(ExternalUseRetainedState&&) noexcept = default;
  // Declared explicitly: the registry context copies any value whose type reports itself
  // copyable, and a vector does even when its elements are not.
  ExternalUseRetainedState(const ExternalUseRetainedState&) = delete;
  ExternalUseRetainedState& operator=(const ExternalUseRetainedState&) = delete;

  std::vector<RetainedSubtree> subtrees;
  std::optional<RenderTreeOccurrence> renderTree;
  /// Root of the occurrence being drawn when it must not outlive its draw, else `entt::null`.
  Entity unretainedRoot = entt::null;
  std::uint64_t draws = 0;
};

template <typename T>
T& GetOrEmplaceContext(Registry& registry) {
  if (T* existing = registry.ctx().find<T>()) {
    return *existing;
  }
  return registry.ctx().emplace<T>();
}

/// Drop \p root's style-derived state (computed styles and stops, layout, text layout) so the next
/// render-tree preparation restyles the subtree under whichever occurrence context, if any, is
/// installed then. Path, paint server and filter data are rebuilt from computed styles on every
/// preparation.
void InvalidateSubtree(Registry& registry, Entity root) {
  if (!registry.valid(root) || !registry.all_of<donner::components::TreeComponent>(root)) {
    return;
  }
  LayoutSystem().invalidate(EntityHandle(registry, root));
  std::vector<Entity> pending{root};
  while (!pending.empty()) {
    const Entity entity = pending.back();
    pending.pop_back();
    StyleSystem().invalidateComputed(EntityHandle(registry, entity));
    registry.remove<ComputedStopComponent>(entity);
    registry.get_or_emplace<DirtyFlagsComponent>(entity).mark(DirtyFlagsComponent::All);
    if (registry.all_of<TextRootComponent>(entity)) {
      (void)InvalidateTextLayout(EntityHandle(registry, entity));
    }
    for (Entity child = registry.get<donner::components::TreeComponent>(entity).firstChild();
         child != entt::null;
         child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
      pending.push_back(child);
    }
  }
  InvalidateFontResourcePreparation(registry);
}

/// Drop every rendering instance and the occurrence's context paint, which may refer to the host.
void DropOccurrenceRenderState(Registry& registry, ExternalUseRetainedState& state) {
  registry.clear<RenderingInstanceComponent>();
  state.renderTree.reset();
  GetOrEmplaceContext<RenderTreeState>(registry).needsInstanceRebuild = true;
  if (auto* context = registry.ctx().find<RenderingContext>()) {
    context->clearInitialContextPaint();
  }
}

bool IsAncestorOrSelf(const Registry& registry, Entity ancestor, Entity entity) {
  while (entity != entt::null && registry.valid(entity)) {
    if (entity == ancestor) {
      return true;
    }
    const auto* tree = registry.try_get<donner::components::TreeComponent>(entity);
    entity = tree ? tree->parent() : entt::null;
  }
  return false;
}

bool Overlaps(const Registry& registry, Entity lhs, Entity rhs) {
  return IsAncestorOrSelf(registry, lhs, rhs) || IsAncestorOrSelf(registry, rhs, lhs);
}

/// Snapshot \p occurrence's inputs, charged to the document family; `std::nullopt` if the family
/// refuses the charge, which leaves the subtree restyled on its next draw.
std::optional<OccurrenceInputs> SnapshotInputs(Registry& registry,
                                               const ExternalUseStyleContext& occurrence) {
  if (!occurrence.parent.properties) {
    return std::nullopt;
  }
  const PropertyRegistry& parent = *occurrence.parent.properties;
  const std::size_t bytes = parent.complexPropertyBytes();
  const auto* context = registry.ctx().find<DocumentResourceFamilyContext>();
  std::shared_ptr<DocumentResourceFamilyBudget> family = context ? context->budget : nullptr;
  if (family && !family->reserve(DocumentResourceFamilyBudget::Kind::ComputedStyle, bytes)) {
    return std::nullopt;
  }
  return OccurrenceInputs{.parent = parent,
                          .resourceOpacities = occurrence.parent.resourceOpacities,
                          .fill = occurrence.fill,
                          .stroke = occurrence.stroke,
                          .charge = RetainedStyleCharge(std::move(family), bytes)};
}

/// Restyle and forget the least recently drawn subtree other than \p keep.
void EvictLeastRecentlyDrawn(Registry& registry, ExternalUseRetainedState& state, Entity keep) {
  auto victim = state.subtrees.end();
  for (auto it = state.subtrees.begin(); it != state.subtrees.end(); ++it) {
    if (it->root != keep && (victim == state.subtrees.end() || it->lastDrawn < victim->lastDrawn)) {
      victim = it;
    }
  }
  if (victim != state.subtrees.end()) {
    InvalidateSubtree(registry, victim->root);
    state.subtrees.erase(victim);
  }
}

std::vector<Entity> StyleDirtyEntities(Registry& registry) {
  std::vector<Entity> entities;
  for (const auto& [entity, dirty] : registry.view<DirtyFlagsComponent>().each()) {
    if (dirty.test(DirtyFlagsComponent::Style)) {
      entities.push_back(entity);
    }
  }
  return entities;
}

/// Whether the next preparation restyles every element: a first build, a whole-document restyle,
/// or any rebuild of an animated document.
bool RestylesWholeDocument(Registry& registry, const RenderTreeState& renderState,
                           bool restylingCurrent) {
  if (!renderState.hasBeenBuilt || renderState.needsFullStyleRecompute) {
    return true;
  }
  const bool rebuilds = restylingCurrent || renderState.needsFullRebuild ||
                        !registry.view<DirtyFlagsComponent>().empty();
  return rebuilds && !registry.view<AnimatedValuesComponent>().empty();
}

/**
 * Forget the inputs of every retained subtree other than \p current's that the next preparation
 * restyles. That restyle runs under the current occurrence alone, so it leaves those subtrees
 * without their own inputs.
 */
void ForgetInputsReachedByRestyle(Registry& registry, ExternalUseRetainedState& state,
                                  const RenderTreeState& renderState, Entity current,
                                  bool restylingCurrent) {
  const bool wholeDocument = RestylesWholeDocument(registry, renderState, restylingCurrent);
  const std::vector<Entity> styleDirty =
      wholeDocument ? std::vector<Entity>() : StyleDirtyEntities(registry);
  for (RetainedSubtree& subtree : state.subtrees) {
    const bool reached =
        wholeDocument || std::any_of(styleDirty.begin(), styleDirty.end(), [&](Entity entity) {
          return IsAncestorOrSelf(registry, subtree.root, entity);
        });
    if (subtree.root != current && reached) {
      subtree.inputs.reset();
    }
  }
}

/// Restyle \p occurrence's subtree for its inputs, forgetting the inputs of every retained subtree
/// the restyle overlaps. Returns the occurrence's entry, which records the inputs only when
/// \p retain is set.
RetainedSubtree& RestyleForOccurrence(Registry& registry, ExternalUseRetainedState& state,
                                      const ExternalUseStyleContext& occurrence, bool retain) {
  InvalidateSubtree(registry, occurrence.root);
  for (RetainedSubtree& other : state.subtrees) {
    if (other.root != occurrence.root && Overlaps(registry, other.root, occurrence.root)) {
      other.inputs.reset();
    }
  }
  auto entry = std::find_if(state.subtrees.begin(), state.subtrees.end(),
                            [&](const RetainedSubtree& s) { return s.root == occurrence.root; });
  if (entry == state.subtrees.end()) {
    if (state.subtrees.size() >= ExternalUseStyleRetention::kMaximumRetainedSubtrees) {
      EvictLeastRecentlyDrawn(registry, state, occurrence.root);
    }
    entry = state.subtrees.insert(state.subtrees.end(), RetainedSubtree{.root = occurrence.root});
  }
  entry->inputs.reset();
  if (retain) {
    entry->inputs = SnapshotInputs(registry, occurrence);
  }
  return *entry;
}

/// Restore \p root's subtree to the document's own styles and forget it.
void RestoreSubtree(Registry& registry, ExternalUseRetainedState& state, Entity root) {
  InvalidateSubtree(registry, root);
  std::erase_if(state.subtrees, [&](const RetainedSubtree& s) { return s.root == root; });
}

/// The element a gradient, pattern or `<use>` inherits from or instantiates, if any.
std::optional<ResolvedReference> ReferencedElement(Registry& registry, Entity entity) {
  if (const auto* gradient = registry.try_get<GradientComponent>(entity);
      gradient && gradient->href) {
    return gradient->href->resolve(registry);
  }
  if (const auto* pattern = registry.try_get<PatternComponent>(entity); pattern && pattern->href) {
    return pattern->href->resolve(registry);
  }
  if (const auto* shadowTree = registry.try_get<ShadowTreeComponent>(entity)) {
    return shadowTree->mainTargetEntity(registry);
  }
  return std::nullopt;
}

/// Whether \p handle names an element of \p registry outside \p root's subtree.
bool IsOutside(const Registry& registry, Entity root, const EntityHandle& handle) {
  return handle && handle.registry() == &registry &&
         !IsAncestorOrSelf(registry, root, handle.entity());
}

bool IsOutside(Registry& registry, Entity root, const std::optional<Reference>& reference) {
  if (!reference) {
    return false;
  }
  const std::optional<ResolvedReference> resolved = reference->resolve(registry);
  return resolved && IsOutside(registry, root, resolved->handle);
}

bool PaintIsOutside(const Registry& registry, Entity root, const ResolvedPaintServer& paint) {
  const auto* reference = std::get_if<PaintResolvedReference>(&paint);
  return reference != nullptr && IsOutside(registry, root, reference->reference.handle);
}

/// Whether a resolved clip path, mask or marker names an element outside \p root's subtree.
template <typename Resolved>
bool ResolvedIsOutside(const Registry& registry, Entity root,
                       const std::optional<Resolved>& value) {
  return value && IsOutside(registry, root, value->reference.handle);
}

/// Whether \p instance resolved a paint server, clip path, mask, marker or filter outside
/// \p root's subtree.
bool InstanceReadsOutside(const Registry& registry, Entity root,
                          const RenderingInstanceComponent& instance) {
  const auto* filter =
      instance.resolvedFilter ? std::get_if<ResolvedReference>(&*instance.resolvedFilter) : nullptr;
  return PaintIsOutside(registry, root, instance.resolvedFill) ||
         PaintIsOutside(registry, root, instance.resolvedStroke) ||
         ResolvedIsOutside(registry, root, instance.clipPath) ||
         ResolvedIsOutside(registry, root, instance.mask) ||
         ResolvedIsOutside(registry, root, instance.markerStart) ||
         ResolvedIsOutside(registry, root, instance.markerMid) ||
         ResolvedIsOutside(registry, root, instance.markerEnd) ||
         (filter != nullptr && IsOutside(registry, root, filter->handle));
}

std::optional<Reference> PaintReference(const std::optional<PaintServer>& paint) {
  const auto* reference =
      paint ? std::get_if<PaintServer::ElementReference>(&paint->value) : nullptr;
  return reference ? std::optional<Reference>(reference->reference) : std::nullopt;
}

/// Whether \p style references a paint server, clip path, mask, marker or filter outside
/// \p root's subtree. Covers elements without rendering instances, such as paint server content.
std::optional<Reference> ClipReference(const std::optional<ClipPath>& clip) {
  const auto* reference = clip ? std::get_if<Reference>(&clip->value) : nullptr;
  return reference ? std::optional<Reference>(*reference) : std::nullopt;
}

bool FilterIsOutside(Registry& registry, Entity root,
                     const std::optional<std::vector<FilterEffect>>& filters) {
  return filters && std::any_of(filters->begin(), filters->end(), [&](const FilterEffect& effect) {
           const auto* reference = std::get_if<FilterEffect::ElementReference>(&effect.value);
           return reference && IsOutside(registry, root, reference->reference);
         });
}

bool StyleReadsOutside(Registry& registry, Entity root, const PropertyRegistry& style) {
  return IsOutside(registry, root, PaintReference(style.fill.get())) ||
         IsOutside(registry, root, PaintReference(style.stroke.get())) ||
         IsOutside(registry, root, ClipReference(style.clipPath.get())) ||
         IsOutside(registry, root, style.mask.get()) ||
         IsOutside(registry, root, style.markerStart.get()) ||
         IsOutside(registry, root, style.markerMid.get()) ||
         IsOutside(registry, root, style.markerEnd.get()) ||
         FilterIsOutside(registry, root, style.filter.get());
}

/// Whether \p entity is an element whose reference this check does not model.
bool IsUnmodeledReference(const Registry& registry, Entity entity) {
  const auto* type = registry.try_get<ElementTypeComponent>(entity);
  return type && (type->type() == ElementType::FeImage || type->type() == ElementType::TextPath ||
                  type->type() == ElementType::MPath);
}

/// Whether \p entity, inside \p root's subtree, reads any element outside that subtree.
bool ElementReadsOutside(Registry& registry, Entity root, Entity entity) {
  if (IsUnmodeledReference(registry, entity)) {
    return true;
  }
  const auto* instance = registry.try_get<RenderingInstanceComponent>(entity);
  const auto* style = registry.try_get<ComputedStyleComponent>(entity);
  const std::optional<ResolvedReference> referenced = ReferencedElement(registry, entity);
  return (instance && InstanceReadsOutside(registry, root, *instance)) ||
         (style && style->properties && StyleReadsOutside(registry, root, *style->properties)) ||
         (referenced && IsOutside(registry, root, referenced->handle));
}

/**
 * Whether drawing \p root's subtree may read any element outside it. Content that an outside
 * element pulls in, such as a pattern's children or the gradients those use, is not followed: any
 * outside reference counts.
 */
bool ReadsOutsideSubtree(Registry& registry, Entity root) {
  std::vector<Entity> pending{root};
  while (!pending.empty()) {
    const Entity entity = pending.back();
    pending.pop_back();
    if (ElementReadsOutside(registry, root, entity)) {
      return true;
    }
    for (Entity child = registry.get<donner::components::TreeComponent>(entity).firstChild();
         child != entt::null;
         child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
      pending.push_back(child);
    }
  }
  return false;
}

/**
 * Whether \p root's subtree holds a `<use>` the installed instance expands but that has no shadow
 * tree, as a definition use is left when the render tree was built for another root. A full
 * rebuild gives every such use a shadow tree component, even one whose expansion is refused for a
 * cycle or a resource limit, so the check does not fire again until another root's rebuild.
 */
bool HasUnexpandedUse(Registry& registry, Entity root) {
  std::vector<Entity> pending{root};
  while (!pending.empty()) {
    const Entity entity = pending.back();
    pending.pop_back();
    const auto* shadowTree = registry.try_get<ShadowTreeComponent>(entity);
    const auto* type = registry.try_get<ElementTypeComponent>(entity);
    if (shadowTree && type && type->type() == ElementType::Use &&
        !registry.all_of<ComputedShadowTreeComponent>(entity) &&
        shadowTree->mainTargetEntity(registry) &&
        !IsDormantDefinitionUse(registry, entity, *shadowTree)) {
      return true;
    }
    for (Entity child = registry.get<donner::components::TreeComponent>(entity).firstChild();
         child != entt::null;
         child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
      pending.push_back(child);
    }
  }
  return false;
}

}  // namespace

void ExternalUseStyleRetention::PrepareOccurrence(Registry& registry) {
  const auto& occurrence = registry.ctx().get<ExternalUseStyleContext>();
  auto& state = GetOrEmplaceContext<ExternalUseRetainedState>(registry);
  auto& renderState = GetOrEmplaceContext<RenderTreeState>(registry);
  const bool retain = IsPlainPaint(occurrence.fill) && IsPlainPaint(occurrence.stroke);

  auto retained = std::find_if(state.subtrees.begin(), state.subtrees.end(),
                               [&](const RetainedSubtree& s) { return s.root == occurrence.root; });
  const bool matches = retain && retained != state.subtrees.end() && retained->inputs &&
                       retained->inputs->matches(occurrence);
  ForgetInputsReachedByRestyle(registry, state, renderState, occurrence.root, !matches);
  RetainedSubtree& entry =
      matches ? *retained : RestyleForOccurrence(registry, state, occurrence, retain);
  entry.lastDrawn = ++state.draws;
  state.unretainedRoot = retain ? entt::null : occurrence.root;
}

void ExternalUseStyleRetention::PrepareInstance(Registry& registry) {
  const auto& occurrence = registry.ctx().get<ExternalUseStyleContext>();
  auto* state = registry.ctx().find<ExternalUseRetainedState>();
  if (state == nullptr) {
    return;
  }
  const auto* sized = registry.try_get<ComputedShadowSizedElementComponent>(occurrence.root);
  const std::optional<Box2d> viewport = sized ? std::optional<Box2d>(sized->bounds) : std::nullopt;
  const bool retain = state->unretainedRoot == entt::null;
  if (retain && state->renderTree && state->renderTree->matches(occurrence, viewport)) {
    return;
  }
  auto& renderState = GetOrEmplaceContext<RenderTreeState>(registry);
  renderState.needsInstanceRebuild = true;
  state->renderTree.reset();
  if (retain) {
    state->renderTree = RenderTreeOccurrence{.root = occurrence.root,
                                             .fill = occurrence.fill,
                                             .stroke = occurrence.stroke,
                                             .viewport = viewport};
  }
  if (HasUnexpandedUse(registry, occurrence.root)) {
    // A rebuild for another root re-expanded only the `<use>` elements that root presents, so
    // this subtree's definition uses need the shadow trees a full rebuild instantiates.
    renderState.needsFullRebuild = true;
    ForgetInputsReachedByRestyle(registry, *state, renderState, occurrence.root,
                                 /*restylingCurrent=*/true);
  }
}

// Only self-contained subtrees keep their retained styles next to others: a sheet whose sprites
// share top-level gradients, patterns or filters restores the other subtrees, and so restyles them,
// for every occurrence that reads outside its own subtree.
bool ExternalUseStyleRetention::RestoreOtherSubtreesIfReadingOutside(Registry& registry) {
  const auto* occurrence = registry.ctx().find<ExternalUseStyleContext>();
  auto* state = registry.ctx().find<ExternalUseRetainedState>();
  if (occurrence == nullptr || state == nullptr || state->subtrees.size() < 2 ||
      !ReadsOutsideSubtree(registry, occurrence->root)) {
    return false;
  }
  const Entity root = occurrence->root;
  for (const RetainedSubtree& subtree : state->subtrees) {
    if (subtree.root != root) {
      InvalidateSubtree(registry, subtree.root);
    }
  }
  std::erase_if(state->subtrees, [&](const RetainedSubtree& s) { return s.root != root; });
  return true;
}

void ExternalUseStyleRetention::FinishOccurrence(Registry& registry) {
  const auto& occurrence = registry.ctx().get<ExternalUseStyleContext>();
  auto* state = registry.ctx().find<ExternalUseRetainedState>();
  if (state == nullptr) {
    return;
  }
  const auto* budget = registry.ctx().find<StyleResourceBudget>();
  const bool limited = budget != nullptr && budget->rejected();
  if (state->unretainedRoot == entt::null && !limited) {
    return;
  }
  state->unretainedRoot = entt::null;
  RestoreSubtree(registry, *state, occurrence.root);
  DropOccurrenceRenderState(registry, *state);
}

void ExternalUseStyleRetention::RestoreStandalone(Registry& registry) {
  const bool drawingOccurrence = registry.ctx().contains<ExternalUseStyleContext>();
  assert(!drawingOccurrence && "RestoreStandalone called while an external use is drawing");
  auto* state = registry.ctx().find<ExternalUseRetainedState>();
  if (drawingOccurrence || state == nullptr) {
    return;
  }
  for (const RetainedSubtree& subtree : state->subtrees) {
    InvalidateSubtree(registry, subtree.root);
  }
  DropOccurrenceRenderState(registry, *state);
  registry.ctx().erase<ExternalUseRetainedState>();
}

void ExternalUseStyleRetention::RestoreStandaloneForRead(Registry& registry) {
  if (!HasOccurrenceState(registry)) {
    return;
  }
  auto* context = registry.ctx().find<RenderingContext>();
  if (context == nullptr) {
    context = &registry.ctx().emplace<RenderingContext>(registry);
  }
  // Preparation diagnostics are dropped here: a read reports values, not warnings. A later verbose
  // render of the document that finds the render tree already prepared does not repeat them.
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  context->instantiateRenderTree(/*verbose=*/false, disabledSink);
}

bool ExternalUseStyleRetention::IsOccurrenceStyled(const Registry& registry, Entity entity) {
  const auto* state = registry.ctx().find<ExternalUseRetainedState>();
  return state != nullptr &&
         std::any_of(state->subtrees.begin(), state->subtrees.end(), [&](const RetainedSubtree& s) {
           return IsAncestorOrSelf(registry, s.root, entity);
         });
}

bool ExternalUseStyleRetention::HasOccurrenceState(const Registry& registry) {
  return registry.ctx().find<ExternalUseRetainedState>() != nullptr;
}

}  // namespace donner::svg::components
