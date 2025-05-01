#pragma once
/// @file

#include <functional>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseError.h"
#include "donner/base/RcString.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowBranch.h"
#include "donner/svg/graph/RecursionGuard.h"

// TODO(jwmcglynn): Automatically delete ComputedShadowTreeComponent when ShadowTreeComponent is
// removed.

namespace donner::svg::components {

/**
 * Type definition for a callback to process sized elements.
 * This allows systems that can't directly depend on LayoutSystem to request
 * sized element processing.
 *
 * @param registry ECS registry.
 * @param shadowEntity The shadow entity to create a computed component for.
 * @param useEntity The source \ref xml_use that may provide size override.
 * @param symbolEntity The target \ref xml_symbol entity whose properties might be overridden.
 * @param branchType The type of branch being created.
 * @param outWarnings Output vector of parse errors, if any.
 * @return true if a component was created, false otherwise.
 */
using ShadowSizedElementHandler = std::function<bool(
    Registry& registry, Entity shadowEntity, EntityHandle useEntity, Entity symbolEntity,
    ShadowBranchType branchType, std::vector<ParseError>* outWarnings)>;

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
   * Constructor.
   *
   * @param sizedElementHandler Optional callback for handling sized elements in shadow trees.
   */
  explicit ShadowTreeSystem(ShadowSizedElementHandler sizedElementHandler = nullptr)
      : sizedElementHandler_(std::move(sizedElementHandler)) {}

  /**
   * Destroy the instantiated shadow tree.
   *
   * @param registry The registry.
   * @param shadow The computed shadow tree component to tear down.
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
   * instances with a shadow tree, but only \ref ShadowBranchType::Main will be traversed in the
   * render tree.
   * @param lightTarget Target entity to reflect in the shadow tree.
   * @param href The value of the href attribute for the shadow tree, for diagnostics.
   * @param outWarnings If provided, warnings will be added to this vector.
   * @returns The index of the offscreen shadow tree, if \p branchType is not the \ref
   * ShadowBranchType::Main branch. Returns \c std::nullopt if it is.
   */
  std::optional<size_t> populateInstance(EntityHandle entity, ComputedShadowTreeComponent& shadow,
                                         ShadowBranchType branchType, Entity lightTarget,
                                         const RcString& href,
                                         std::vector<ParseError>* outWarnings);

private:
  Entity createShadowEntity(Registry& registry, ShadowBranchType branchType,
                            ComputedShadowTreeComponent::BranchStorage& storage, Entity lightTarget,
                            Entity shadowParent);

  Entity createShadowAndChildren(Registry& registry, ShadowBranchType branchType,
                                 ComputedShadowTreeComponent::BranchStorage& storage,
                                 RecursionGuard& guard, Entity shadowParent, Entity lightTarget,
                                 const std::set<Entity>& shadowHostParents,
                                 std::vector<ParseError>* outWarnings);

private:
  /// Callback for sized element processing, may be nullptr
  ShadowSizedElementHandler sizedElementHandler_;
};

}  // namespace donner::svg::components
