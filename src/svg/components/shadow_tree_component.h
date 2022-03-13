#pragma once

#include "src/base/rc_string.h"
#include "src/svg/components/document_context.h"
#include "src/svg/graph/reference.h"
#include "src/svg/registry/registry.h"

namespace donner::svg {

class ShadowTreeComponent {
public:
  explicit ShadowTreeComponent(Reference href) : reference_(href) {}

  RcString href() const { return reference_.href; }

  std::optional<ResolvedReference> targetEntity(Registry& registry) {
    return reference_.resolve(registry);
  }

  bool setsContextColors = false;

private:
  Reference reference_;
};

}  // namespace donner::svg
