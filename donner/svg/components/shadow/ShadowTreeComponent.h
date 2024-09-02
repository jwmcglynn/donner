#pragma once
/// @file

#include "donner/base/RcString.h"
#include "donner/base/RcStringOrRef.h"
#include "donner/svg/graph/Reference.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg::components {

/**
 * Indicates the entry point to a shadow tree, which instantiates a virtual tree of entities
 * mirroring another entity's tree.
 *
 * For more information about shadow trees:
 * - For \ref xml_use elements: https://www.w3.org/TR/SVG2/struct.html#UseShadowTree
 * - For paint servers such as \ref xml_pattern elements:
 * https://www.w3.org/TR/SVG2/pservers.html#PaintServerTemplates
 *
 * Note that only \ref xml_use elements are true shadow trees, paint servers are technically
 * [re-used graphics](https://www.w3.org/TR/SVG2/render.html#Definitions).
 *
 * To use, create and call \ref ShadowTreeComponent::setMainHref.
 *
 * When instantiated, creates a \ref BranchType::Main shadow tree, which is the main render graph.
 * For other shadow trees, see \ref OffscreenShadowTreeComponent.
 */
class ShadowTreeComponent {
public:
  /// Constructor.
  ShadowTreeComponent() = default;

  /**
   * Get the href attribute for the shadow tree target.
   *
   * @returns The href attribute, or std::nullopt if not set.
   */
  std::optional<RcString> mainHref() const {
    return mainReference_ ? std::make_optional(mainReference_->href) : std::nullopt;
  }

  /**
   * Set the href attribute for the shadow tree target, which must be an element reference (e.g.
   * "#otherEntity").
   *
   * @param href The href attribute value.
   */
  void setMainHref(const RcStringOrRef& href) { mainReference_ = Reference(RcString(href)); }

  /**
   * Get the resolved entity for the main target of the shadow tree, if the \ref mainHref was able
   * to be resolved.
   *
   * @param registry The registry.
   * @returns The resolved entity, or std::nullopt if not set.
   */
  std::optional<ResolvedReference> mainTargetEntity(Registry& registry) const {
    return mainReference_ ? mainReference_->resolve(registry) : std::nullopt;
  }

  /// Whether this shadow tree inherits the CSS `context-color` from the parent tree.
  bool setsContextColors = false;

private:
  /// The reference to the main target of the shadow tree.
  std::optional<Reference> mainReference_;
};

}  // namespace donner::svg::components
