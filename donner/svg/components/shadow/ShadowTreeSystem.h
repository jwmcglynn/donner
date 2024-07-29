#pragma once
/// @file

#include "donner/svg/components/TreeComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/OffscreenShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowBranch.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/DoNotInheritFillOrStrokeTag.h"
#include "donner/svg/graph/RecursionGuard.h"
#include "donner/svg/registry/Registry.h"

// TODO: Automatically delete ComputedShadowTreeComponent when ShadowTreeComponent is removed.

namespace donner::svg::components {

/**
 * Instantiates shadow trees for elements that are not part of the main render graph, such as
 * \ref xml_use and \ref xml_pattern elements.
 *
 * @ingroup ecs_systems
 * @see https://www.w3.org/TR/SVG2/struct.html#UseShadowTree
 * @see https://www.w3.org/TR/SVG2/pservers.html#PaintServerTemplates
 */
class ShadowTreeSystem {
public:
  /**
   * Destroy the instantiated shadow tree.
   *
   * @param registry The registry.
   */
  void teardown(Registry& registry, ComputedShadowTreeComponent& shadow);

  /**
   * Create a new computed shadow tree instance, such as the shadow tree for a \ref xml_use element
   * or a \ref xml_pattern element.
   *
   * For \ref xml_pattern paint servers, there may be multiple shadow trees originating from the
   * same entity, for both a 'fill' and a 'stroke', so this component can hold multiple shadow trees
   * simultaneously.
   *
   * @param entity The entity to create the shadow tree for.
   * @param shadow The computed shadow tree component to populate.
   * @param branchType Determines which branch of the tree to attach to. There may be multiple
   *   instances with a shadow tree, but only \ref ShadowBranchType::Main will be traversed in the
   * render tree.
   * @param lightTarget Target entity to reflect in the shadow tree.
   * @param href The value of the href attribute for the shadow tree, for diagnostics.
   * @param outWarnings If provided, warnings will be added to this vector.
   * @returns The index of the offscreen shadow tree, if \ref branchType is \ref
   *   ShadowBranchType::HiddenOffscreen, or std::nullopt otherwise.
   */
  std::optional<size_t> populateInstance(EntityHandle entity, ComputedShadowTreeComponent& shadow,
                                         ShadowBranchType branchType, Entity lightTarget,
                                         const RcString& href,
                                         std::vector<parser::ParseError>* outWarnings);

private:
  Entity createShadowEntity(Registry& registry, ShadowBranchType branchType,
                            ComputedShadowTreeComponent::BranchStorage& storage, Entity lightTarget,
                            Entity shadowParent);

  void computeChildren(Registry& registry, ShadowBranchType branchType,
                       ComputedShadowTreeComponent::BranchStorage& storage, RecursionGuard& guard,
                       Entity shadowParent, Entity lightTarget,
                       const std::set<Entity>& shadowHostParents,
                       std::vector<parser::ParseError>* outWarnings);
};

}  // namespace donner::svg::components
