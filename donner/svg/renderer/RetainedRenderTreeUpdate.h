#pragma once
/// @file
/// Retained render-tree updates: apply a frame's DOM changes without tearing down and recomputing
/// the whole document, when every change is one the computed components do not depend on.

#include <optional>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/RcString.h"

namespace donner::svg::components {

/**
 * The work a retained update performs, collected while checking that a frame's dirty set
 * qualifies for one.
 *
 * A dirty set qualifies when every change only moves content (a transform write) or points an
 * external `<use>` at another external instance. Neither changes any input of the computed
 * styles, the shadow trees, the computed paths, text layout, paint servers or filter graphs, so
 * those are kept, and the render instances are rebuilt from them. Anything else, or any case where
 * one of these changes could reach a computed component (for example a moved element that a shadow
 * tree clones, or that a
 * `<textPath>` follows), takes the full rebuild.
 */
struct RetainedUpdatePlan {
  /// Entities whose computed layout was dropped by a transform write.
  std::vector<Entity> layoutEntities;
  /// External `<use>` elements that were retargeted to another external instance.
  std::vector<Entity> retargetedUses;
};

/**
 * Check whether the current dirty set of \p registry qualifies for a retained update, and plan it.
 * The caller must already know the render tree was built and that no full rebuild or whole-tree
 * restyle was requested.
 *
 * @param registry Document registry.
 * @return The plan, or `std::nullopt` when the frame needs the full rebuild.
 */
std::optional<RetainedUpdatePlan> PlanRetainedUpdate(Registry& registry);

/**
 * Bring the computed components of \p registry up to date for a frame planned by \ref
 * PlanRetainedUpdate. The render instances are rebuilt afterwards, from the updated components.
 *
 * @param registry Document registry.
 * @param plan The plan returned by \ref PlanRetainedUpdate for the current dirty set.
 * @param warningSink Sink to collect warnings.
 */
void ApplyRetainedUpdate(Registry& registry, const RetainedUpdatePlan& plan,
                         ParseWarningSink& warningSink);

/**
 * Give an external `<use>` the document its reference names, as an \ref ExternalUseComponent.
 * The document is admitted to the resource family once per full rebuild, and again when the
 * document's families change, see \ref ResourceManagerContext::loadAdmittedExternalSVG.
 *
 * @param registry Document registry.
 * @param entity The `<use>` element.
 * @param href The use's external reference.
 * @param warningSink Sink to collect warnings.
 */
void AttachExternalUse(Registry& registry, Entity entity, const RcString& href,
                       ParseWarningSink& warningSink);

}  // namespace donner::svg::components
