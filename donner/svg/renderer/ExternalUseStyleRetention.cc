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
#include "donner/svg/components/FontResourceGraph.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/style/ExternalUseStyleContext.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/components/text/TextInvalidation.h"
#include "donner/svg/components/text/TextRootComponent.h"

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

bool HasStyleDirtyEntity(Registry& registry) {
  for (const auto& [entity, dirty] : registry.view<DirtyFlagsComponent>().each()) {
    if (dirty.test(DirtyFlagsComponent::Style)) {
      return true;
    }
  }
  return false;
}

/**
 * Forget the inputs of every retained subtree except \p current's when the next preparation may
 * restyle elements outside \p current's subtree: a first build, a whole-document restyle, an
 * element a mutation flagged for restyle, or any rebuild of an animated document, which restyles
 * everything. That restyle runs under the current occurrence alone, so it leaves other subtrees
 * without their own inputs.
 */
void ForgetInputsReachedByRestyle(Registry& registry, ExternalUseRetainedState& state,
                                  const RenderTreeState& renderState, Entity current,
                                  bool restylingCurrent) {
  const bool rebuilds = restylingCurrent || renderState.needsFullRebuild ||
                        !registry.view<DirtyFlagsComponent>().empty();
  const bool restylesEverything = !renderState.hasBeenBuilt ||
                                  renderState.needsFullStyleRecompute ||
                                  HasStyleDirtyEntity(registry) ||
                                  (rebuilds && !registry.view<AnimatedValuesComponent>().empty());
  if (!restylesEverything) {
    return;
  }
  for (RetainedSubtree& subtree : state.subtrees) {
    if (subtree.root != current) {
      subtree.inputs.reset();
    }
  }
}

/// Restyle \p occurrence's subtree for its inputs, forgetting the inputs of every retained subtree
/// the restyle overlaps. Returns the occurrence's entry.
RetainedSubtree& RestyleForOccurrence(Registry& registry, ExternalUseRetainedState& state,
                                      const ExternalUseStyleContext& occurrence) {
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
  entry->inputs = SnapshotInputs(registry, occurrence);
  return *entry;
}

}  // namespace

void ExternalUseStyleRetention::PrepareOccurrence(Registry& registry) {
  const auto& occurrence = registry.ctx().get<ExternalUseStyleContext>();
  auto& state = GetOrEmplaceContext<ExternalUseRetainedState>(registry);
  auto& renderState = GetOrEmplaceContext<RenderTreeState>(registry);

  auto retained = std::find_if(state.subtrees.begin(), state.subtrees.end(),
                               [&](const RetainedSubtree& s) { return s.root == occurrence.root; });
  const bool matches =
      retained != state.subtrees.end() && retained->inputs && retained->inputs->matches(occurrence);
  ForgetInputsReachedByRestyle(registry, state, renderState, occurrence.root, !matches);
  RetainedSubtree& entry = matches ? *retained : RestyleForOccurrence(registry, state, occurrence);
  entry.lastDrawn = ++state.draws;

  if (!state.renderTree || !state.renderTree->matches(occurrence)) {
    renderState.needsInstanceRebuild = true;
    state.renderTree = RenderTreeOccurrence{
        .root = occurrence.root, .fill = occurrence.fill, .stroke = occurrence.stroke};
  }
}

bool ExternalUseStyleRetention::RestoreStandalone(Registry& registry) {
  const auto* state = registry.ctx().find<ExternalUseRetainedState>();
  if (state == nullptr) {
    return false;
  }
  for (const RetainedSubtree& subtree : state->subtrees) {
    InvalidateSubtree(registry, subtree.root);
  }
  registry.ctx().erase<ExternalUseRetainedState>();
  GetOrEmplaceContext<RenderTreeState>(registry).needsInstanceRebuild = true;
  return true;
}

}  // namespace donner::svg::components
