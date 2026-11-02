#include "donner/svg/renderer/ExternalUseStyleRetention.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/FontResourceGraph.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
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

  bool matches(const ExternalUseStyleContext& occurrence) const {
    return root == occurrence.root && IsProvablySamePaint(fill, occurrence.fill) &&
           IsProvablySamePaint(stroke, occurrence.stroke);
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

/// Drop \p root's computed styles and text layout so the next render-tree preparation restyles the
/// subtree under whichever occurrence context, if any, is installed then.
void InvalidateSubtree(Registry& registry, Entity root) {
  if (!registry.valid(root) || !registry.all_of<donner::components::TreeComponent>(root)) {
    return;
  }
  std::vector<Entity> pending{root};
  while (!pending.empty()) {
    const Entity entity = pending.back();
    pending.pop_back();
    StyleSystem().invalidateComputed(EntityHandle(registry, entity));
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

/// Entities outside an occurrence's subtree that drawing it reads.
struct ReadTargets {
  std::vector<Entity> entities;
  /// Set when the subtree has a reference these targets cannot describe.
  bool unbounded = false;
};

void AddTarget(const Registry& registry, Entity root, const EntityHandle& handle,
               ReadTargets& targets) {
  if (!handle || handle.registry() != &registry ||
      IsAncestorOrSelf(registry, root, handle.entity()) ||
      std::find(targets.entities.begin(), targets.entities.end(), handle.entity()) !=
          targets.entities.end()) {
    return;
  }
  targets.entities.push_back(handle.entity());
}

void AddPaintTarget(const Registry& registry, Entity root, const ResolvedPaintServer& paint,
                    ReadTargets& targets) {
  if (const auto* reference = std::get_if<PaintResolvedReference>(&paint)) {
    AddTarget(registry, root, reference->reference.handle, targets);
  }
}

void AddInstanceTargets(const Registry& registry, Entity root,
                        const RenderingInstanceComponent& instance, ReadTargets& targets) {
  AddPaintTarget(registry, root, instance.resolvedFill, targets);
  AddPaintTarget(registry, root, instance.resolvedStroke, targets);
  if (instance.clipPath) {
    AddTarget(registry, root, instance.clipPath->reference.handle, targets);
  }
  if (instance.mask) {
    AddTarget(registry, root, instance.mask->reference.handle, targets);
  }
  for (const auto* marker : {&instance.markerStart, &instance.markerMid, &instance.markerEnd}) {
    if (*marker) {
      AddTarget(registry, root, (*marker)->reference.handle, targets);
    }
  }
  if (instance.resolvedFilter) {
    if (const auto* reference = std::get_if<ResolvedReference>(&*instance.resolvedFilter)) {
      AddTarget(registry, root, reference->handle, targets);
    }
  }
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

void AddElementTargets(Registry& registry, Entity root, Entity entity, ReadTargets& targets) {
  if (const auto* type = registry.try_get<ElementTypeComponent>(entity);
      type && (type->type() == ElementType::FeImage || type->type() == ElementType::TextPath ||
               type->type() == ElementType::MPath)) {
    targets.unbounded = true;
    return;
  }
  if (const auto* instance = registry.try_get<RenderingInstanceComponent>(entity)) {
    AddInstanceTargets(registry, root, *instance, targets);
  }
  if (const std::optional<ResolvedReference> referenced = ReferencedElement(registry, entity)) {
    AddTarget(registry, root, referenced->handle, targets);
  }
}

/// Collect what drawing \p root's subtree reads outside it, following the inheritance chains of
/// the referenced paint servers.
ReadTargets CollectReadTargets(Registry& registry, Entity root) {
  ReadTargets targets;
  std::vector<Entity> pending{root};
  while (!pending.empty() && !targets.unbounded) {
    const Entity entity = pending.back();
    pending.pop_back();
    AddElementTargets(registry, root, entity, targets);
    for (Entity child = registry.get<donner::components::TreeComponent>(entity).firstChild();
         child != entt::null;
         child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
      pending.push_back(child);
    }
  }
  for (std::size_t i = 0; i < targets.entities.size() && !targets.unbounded; ++i) {
    if (const std::optional<ResolvedReference> referenced =
            ReferencedElement(registry, targets.entities[i])) {
      AddTarget(registry, root, referenced->handle, targets);
    }
  }
  return targets;
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

  if (!retain || !state.renderTree || !state.renderTree->matches(occurrence)) {
    renderState.needsInstanceRebuild = true;
    state.renderTree.reset();
    if (retain) {
      state.renderTree = RenderTreeOccurrence{
          .root = occurrence.root, .fill = occurrence.fill, .stroke = occurrence.stroke};
    }
  }
}

bool ExternalUseStyleRetention::RestoreSubtreesReadByOccurrence(Registry& registry) {
  const auto* occurrence = registry.ctx().find<ExternalUseStyleContext>();
  auto* state = registry.ctx().find<ExternalUseRetainedState>();
  if (occurrence == nullptr || state == nullptr || state->subtrees.size() < 2) {
    return false;
  }
  const Entity root = occurrence->root;
  const ReadTargets targets = CollectReadTargets(registry, root);
  std::vector<Entity> restore;
  for (const RetainedSubtree& subtree : state->subtrees) {
    const bool read =
        targets.unbounded ||
        std::any_of(targets.entities.begin(), targets.entities.end(),
                    [&](Entity target) { return Overlaps(registry, subtree.root, target); });
    if (subtree.root != root && read) {
      restore.push_back(subtree.root);
    }
  }
  for (Entity subtreeRoot : restore) {
    RestoreSubtree(registry, *state, subtreeRoot);
  }
  return !restore.empty();
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
  auto* state = registry.ctx().find<ExternalUseRetainedState>();
  if (state == nullptr) {
    return;
  }
  for (const RetainedSubtree& subtree : state->subtrees) {
    InvalidateSubtree(registry, subtree.root);
  }
  DropOccurrenceRenderState(registry, *state);
  registry.ctx().erase<ExternalUseRetainedState>();
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
