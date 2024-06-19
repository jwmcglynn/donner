#pragma once
/// @file

#include "donner/base/RcString.h"
#include "donner/base/RcStringOrRef.h"
#include "donner/svg/graph/Reference.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg::components {

class ShadowTreeComponent {
public:
  ShadowTreeComponent() = default;

  std::optional<RcString> mainHref() const {
    return mainReference_ ? std::make_optional(mainReference_->href) : std::nullopt;
  }

  void setMainHref(const RcStringOrRef& href) { mainReference_ = Reference(RcString(href)); }

  std::optional<ResolvedReference> mainTargetEntity(Registry& registry) const {
    return mainReference_ ? mainReference_->resolve(registry) : std::nullopt;
  }

  bool setsContextColors = false;

private:
  std::optional<Reference> mainReference_;
};

}  // namespace donner::svg::components
