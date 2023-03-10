#pragma once
/// @file

#include <span>

#include "src/base/rc_string.h"
#include "src/svg/core/shadow_branch.h"
#include "src/svg/graph/reference.h"
#include "src/svg/registry/registry.h"

namespace donner::svg {

class OffscreenShadowTreeComponent {
public:
  OffscreenShadowTreeComponent() = default;

  std::optional<RcString> branchHref(ShadowBranchType branchType) const {
    const auto it = branches_.find(branchType);
    if (it != branches_.end()) {
      return std::make_optional(it->second.href);
    } else {
      return std::nullopt;
    }
  }

  void setBranchHref(ShadowBranchType branchType, RcString href) {
    assert(branchType != ShadowBranchType::Main);
    branches_.emplace(branchType, href);
  }

  std::optional<ResolvedReference> branchTargetEntity(Registry& registry,
                                                      ShadowBranchType branchType) const {
    const auto it = branches_.find(branchType);
    if (it != branches_.end()) {
      return it->second.resolve(registry);
    } else {
      return std::nullopt;
    }
  }

  const std::map<ShadowBranchType, Reference> branches() const { return branches_; }

private:
  std::map<ShadowBranchType, Reference> branches_;
};

}  // namespace donner::svg
