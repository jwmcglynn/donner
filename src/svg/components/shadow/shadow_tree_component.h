#pragma once
/// @file

#include "src/base/rc_string.h"
#include "src/base/rc_string_or_ref.h"
#include "src/svg/graph/reference.h"
#include "src/svg/registry/registry.h"

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
