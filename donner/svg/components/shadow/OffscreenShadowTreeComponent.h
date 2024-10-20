#pragma once
/// @file

#include <span>

#include "donner/base/EcsRegistry.h"
#include "donner/base/RcString.h"
#include "donner/svg/components/shadow/ShadowBranch.h"
#include "donner/svg/graph/Reference.h"

namespace donner::svg::components {

/**
 * Defines an offscreen shadow tree attached to the current entity (the shadow host).
 *
 * An offscreen shadow tree is a tree of entities, outside of the main render tree, which are
 * rendered in the process of compositing the current entity. This is used for paint servers, which
 * can be instantiated for `fill` or `stroke` attributes.
 *
 * Supported shadow tree types are defined by \ref ShadowBranchType.
 */
class OffscreenShadowTreeComponent {
public:
  /// Default constructor.
  OffscreenShadowTreeComponent() = default;

  /**
   * Get the href attribute for the shadow tree target, e.g. "#otherEntity".
   *
   * @param branchType The branch type to get the href for.
   * @returns The href attribute, or \c std::nullopt if not set.
   */
  std::optional<RcString> branchHref(ShadowBranchType branchType) const {
    const auto it = branches_.find(branchType);
    if (it != branches_.end()) {
      return std::make_optional(it->second.href);
    } else {
      return std::nullopt;
    }
  }

  /**
   * Set the href attribute for the shadow tree target, which must be an element reference (e.g.
   * "#otherEntity").
   *
   * @param branchType The branch type to set the href for.
   * @param href The href attribute value.
   */
  void setBranchHref(ShadowBranchType branchType, const RcString& href) {
    assert(branchType != ShadowBranchType::Main);
    branches_.emplace(branchType, href);
  }

  /**
   * Get the resolved entity for the target of the shadow tree, if the href was able to be resolved.
   *
   * @param registry The registry.
   * @param branchType The branch type to get the resolved entity for.
   * @returns The resolved entity, or \c std::nullopt if not set.
   */
  std::optional<ResolvedReference> branchTargetEntity(Registry& registry,
                                                      ShadowBranchType branchType) const {
    const auto it = branches_.find(branchType);
    if (it != branches_.end()) {
      return it->second.resolve(registry);
    } else {
      return std::nullopt;
    }
  }

  /**
   * Get the underlying map containing each branch and reference.
   */
  const std::map<ShadowBranchType, Reference>& branches() const { return branches_; }

private:
  /// Contains all of the branches for this shadow tree.
  std::map<ShadowBranchType, Reference> branches_;
};

}  // namespace donner::svg::components
