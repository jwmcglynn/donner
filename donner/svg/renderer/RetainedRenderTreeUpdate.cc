#include "donner/svg/renderer/RetainedRenderTreeUpdate.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "donner/base/RcString.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/FontPaintDependenciesComponent.h"
#include "donner/svg/components/GeometryPreparationResourceBudget.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/animation/AnimationTimingComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowBranch.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/components/text/TextPathComponent.h"
#include "donner/svg/graph/Reference.h"

namespace donner::svg::components {
namespace {

/// Changes that move content: the transform write itself and what it propagates to descendants.
constexpr uint16_t kMoveFlags = DirtyFlagsComponent::Layout | DirtyFlagsComponent::Transform |
                                DirtyFlagsComponent::WorldTransform |
                                DirtyFlagsComponent::RenderInstance;

/// Every change a retained update can apply.
constexpr uint16_t kRetainedFlags =
    kMoveFlags | DirtyFlagsComponent::Style | DirtyFlagsComponent::Compositing |
    DirtyFlagsComponent::ShadowTree | DirtyFlagsComponent::ReferencedContent |
    DirtyFlagsComponent::NarrowedStyle;

/// Changes to an element's own inputs, as opposed to an ancestor's transform.
constexpr uint16_t kOwnChangeFlags = DirtyFlagsComponent::Transform |
                                     DirtyFlagsComponent::Compositing |
                                     DirtyFlagsComponent::ShadowTree;

/// Facts about the whole document that the per-entity checks consult.
struct DocumentFacts {
  /// Light entities that some shadow tree clones: a clone copies their transform and style when
  /// it is created, so a change to them needs the shadow tree rebuilt.
  std::unordered_set<Entity> shadowSources;
  /// Whether any `<textPath>` exists; text laid out along a path follows that path's transform.
  bool hasTextPaths = false;
};

/// `feImage` fragment instances are recreated by every draw and only a full rebuild retires them,
/// so documents that have them keep the full rebuild.
bool HasFeImageShadowTrees(const Registry& registry) {
  for (const auto& [entity, shadow] : registry.view<const ComputedShadowTreeComponent>().each()) {
    if (shadow.findOffscreenShadow(ShadowBranchType::OffscreenFeImage).has_value()) {
      return true;
    }
  }
  return false;
}

/// Animation rewrites computed values from their unanimated base on every full pass.
bool DocumentAllowsRetainedUpdate(const Registry& registry) {
  return registry.view<const AnimationTimingComponent>().empty() &&
         !HasFeImageShadowTrees(registry);
}

/// Collect \ref DocumentFacts; the shadow-tree sources only when \p withShadowSources is set.
DocumentFacts CollectDocumentFacts(const Registry& registry, bool withShadowSources) {
  DocumentFacts facts;
  if (withShadowSources) {
    for (const auto& [entity, shadowEntity] : registry.view<const ShadowEntityComponent>().each()) {
      facts.shadowSources.insert(shadowEntity.lightEntity);
    }
  }
  facts.hasTextPaths = !registry.view<const TextPathComponent>().empty();
  return facts;
}

/// Whether \p entity is a use that draws an external instance and owns no shadow tree.
bool IsExternalOnlyUse(const Registry& registry, Entity entity) {
  const auto* shadowTree = registry.try_get<ShadowTreeComponent>(entity);
  if (shadowTree == nullptr || registry.all_of<ComputedShadowTreeComponent>(entity)) {
    return false;
  }
  const std::optional<RcString> href = shadowTree->mainHref();
  return href.has_value() && Reference(*href).isExternal();
}

bool Has(uint16_t flags, uint16_t flag) {
  return (flags & flag) != 0;
}

/// Whether \p flags only name changes a retained update applies.
bool FlagsAreRetainable(uint16_t flags) {
  return (flags & ~kRetainedFlags) == 0 &&
         (!Has(flags, DirtyFlagsComponent::Style) ||
          Has(flags, DirtyFlagsComponent::Compositing | DirtyFlagsComponent::NarrowedStyle));
}

/// Whether no computed component reads the changed inputs of \p entity.
bool ChangeStaysLocal(const Registry& registry, Entity entity, uint16_t flags,
                      const DocumentFacts& facts) {
  if (Has(flags, kOwnChangeFlags) && facts.shadowSources.contains(entity)) {
    return false;
  }
  // A moved element that is not drawn directly is read by whatever references it.
  return !Has(flags, DirtyFlagsComponent::Transform) ||
         (!facts.hasTextPaths && registry.all_of<RenderingInstanceComponent>(entity));
}

/// Check one dirty entity against the retained-update rules, adding its work to \p plan.
bool PlanDirtyEntity(const Registry& registry, Entity entity, uint16_t flags,
                     const DocumentFacts& facts, RetainedUpdatePlan& plan) {
  if (!FlagsAreRetainable(flags) || !ChangeStaysLocal(registry, entity, flags, facts)) {
    return false;
  }
  if (Has(flags, DirtyFlagsComponent::Layout)) {
    // A viewport change reaches every descendant's lengths, and recomputing the layout reads the
    // entity's computed style.
    // An entity also flagged Style has its computed style recreated by the selective restyle,
    // which ApplyRetainedUpdate runs before recomputing layout.
    if (registry.all_of<ViewBoxComponent>(entity) ||
        (!registry.all_of<ComputedStyleComponent>(entity) &&
         !Has(flags, DirtyFlagsComponent::Style))) {
      return false;
    }
    plan.layoutEntities.push_back(entity);
  }
  if (Has(flags, DirtyFlagsComponent::ShadowTree)) {
    if (!IsExternalOnlyUse(registry, entity)) {
      return false;
    }
    plan.retargetedUses.push_back(entity);
  }
  plan.restyle |= Has(flags, DirtyFlagsComponent::Style);
  return true;
}

}  // namespace

std::optional<RetainedUpdatePlan> PlanRetainedUpdate(Registry& registry) {
  if (!DocumentAllowsRetainedUpdate(registry)) {
    return std::nullopt;
  }
  // Refuse any other change before scanning the document's shadow trees, which only an element's
  // own change needs.
  const auto* state = registry.ctx().find<RenderTreeState>();
  const uint16_t supported = state && state->applyLiveStyleUpdate
                                 ? kRetainedFlags
                                 : kRetainedFlags & ~DirtyFlagsComponent::NarrowedStyle;
  bool ownChanges = false;
  for (const auto& [entity, dirty] : registry.view<const DirtyFlagsComponent>().each()) {
    if ((dirty.flags & ~supported) != 0) {
      return std::nullopt;
    }
    ownChanges |= Has(dirty.flags, kOwnChangeFlags);
  }
  const DocumentFacts facts = CollectDocumentFacts(registry, ownChanges);
  RetainedUpdatePlan plan;
  for (const auto& [entity, dirty] : registry.view<const DirtyFlagsComponent>().each()) {
    if (!PlanDirtyEntity(registry, entity, dirty.flags, facts, plan)) {
      return std::nullopt;
    }
  }
  return plan;
}

bool ApplyRetainedUpdate(Registry& registry, const RetainedUpdatePlan& plan,
                         ParseWarningSink& warningSink, StylePassScheduler scheduler) {
  // With the render tree built and no whole-tree restyle requested, this is the selective pass:
  // it restyles only the entities flagged Style.
  if (plan.restyle) {
    if (const auto apply = registry.ctx().get<RenderTreeState>().applyLiveStyleUpdate) {
      if (!apply(registry, warningSink, scheduler)) {
        return false;
      }
    } else {
      StyleSystem().computeAllStyles(registry, warningSink,
                                     StyleSystem::FullRestyle::DiscardAndRecompute, scheduler);
    }
  }
  for (const Entity entity : plan.layoutEntities) {
    LayoutSystem().recomputeInvalidatedLayout(EntityHandle(registry, entity), warningSink);
  }
  for (const Entity entity : plan.retargetedUses) {
    // The previous instance's font dependencies no longer apply; drawing records the new ones.
    registry.remove<FontPaintDependenciesComponent>(entity);
    AttachExternalUse(registry, entity,
                      registry.get<ShadowTreeComponent>(entity).mainHref().value(), warningSink);
  }
  return true;
}

namespace {

bool GeometryBudgetAllowsRetainedStyle(const Registry& registry) {
  if (const auto* budget = registry.ctx().find<GeometryPreparationResourceBudget>();
      budget && (budget->rejected() || budget->limits().maximumBytes !=
                                           GeometryPreparationResourceBudget::kMaximumBytes)) {
    return false;
  }
  const auto* family = registry.ctx().find<DocumentResourceFamilyContext>();
  if (!family || !family->budget) {
    return true;
  }
  const DocumentResourceFamilyBudget::Limits defaults;
  const auto& limits = family->budget->limits();
  return limits.geometryBytes == defaults.geometryBytes &&
         limits.maximumTotalRetainedBytes == defaults.maximumTotalRetainedBytes &&
         family->budget->canReserve(DocumentResourceFamilyBudget::Kind::Geometry, 0);
}

bool CollectNarrowedStyleEntities(const Registry& registry, std::vector<Entity>& narrowed) {
  constexpr uint16_t kNarrowedFlags =
      DirtyFlagsComponent::NarrowedStyle | DirtyFlagsComponent::Style |
      DirtyFlagsComponent::RenderInstance | DirtyFlagsComponent::ReferencedContent;
  for (const auto& [entity, dirty] : registry.view<const DirtyFlagsComponent>().each()) {
    if (dirty.test(DirtyFlagsComponent::NarrowedStyle)) {
      if (!dirty.test(DirtyFlagsComponent::Style) || (dirty.flags & ~kNarrowedFlags) != 0 ||
          registry.all_of<ViewBoxComponent>(entity)) {
        return false;
      }
      narrowed.push_back(entity);
    }
  }
  return true;
}

}  // namespace

bool ApplyLiveStyleUpdate(Registry& registry, ParseWarningSink& warningSink,
                          StylePassScheduler scheduler) {
  std::vector<Entity> narrowed;
  if (!CollectNarrowedStyleEntities(registry, narrowed)) {
    return false;
  }
  if (narrowed.empty()) {
    StyleSystem().computeAllStyles(registry, warningSink,
                                   StyleSystem::FullRestyle::DiscardAndRecompute, scheduler);
    return true;
  }
  if (!GeometryBudgetAllowsRetainedStyle(registry)) {
    return false;
  }
  auto tentativeWarnings = warningSink.remainingAllowance();
  StyleSystem().computeAllStyles(registry, tentativeWarnings,
                                 StyleSystem::FullRestyle::DiscardAndRecompute, scheduler);
  const bool stillLocal = std::ranges::all_of(narrowed, [&](Entity entity) {
    return StyleSystem::retainedStyleOrder(registry, entity) ==
           registry.get<DirtyFlagsComponent>(entity).narrowedStyleOrder;
  });
  const bool styleRefused = registry.ctx().get<StyleResourceBudget>().rejected();
  if (!stillLocal || styleRefused) {
    // Warning-free light styles can feed the full render rebuild directly. If a style
    // diagnostic or refusal occurred, recompute it in the normal full-pass order.
    if (tentativeWarnings.warningAttempts() || styleRefused) {
      registry.ctx().get<RenderTreeState>().needsFullStyleRecompute = true;
    }
    return false;
  }
  const bool styleDiagnostics = tentativeWarnings.warningAttempts() != 0;
  for (Entity entity : narrowed) {
    LayoutSystem().recomputeInvalidatedLayout(EntityHandle(registry, entity), tentativeWarnings);
  }
  if (!ShapeSystem().recomputeStyledPaths(registry, narrowed, tentativeWarnings)) {
    registry.ctx().get<RenderTreeState>().needsFullStyleRecompute |= styleDiagnostics;
    return false;
  }
  warningSink.merge(std::move(tentativeWarnings));
  return true;
}

void AttachExternalUse(Registry& registry, Entity entity, const RcString& href,
                       ParseWarningSink& warningSink) {
  const Reference ref(href);
  auto& resourceManager = registry.ctx().get<ResourceManagerContext>();
  if (std::optional<SVGDocumentHandle> subDocument =
          resourceManager.loadAdmittedExternalSVG(ref.documentUrl(), warningSink)) {
    registry.emplace_or_replace<ExternalUseComponent>(entity, std::move(*subDocument),
                                                      RcString(ref.fragment()));
  } else {
    // A refused admission must not leave an earlier instance of the document drawing.
    registry.remove<ExternalUseComponent>(entity);
  }
}

}  // namespace donner::svg::components
