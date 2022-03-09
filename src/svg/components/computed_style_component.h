#pragma once

#include "src/svg/properties/property_registry.h"
#include "src/svg/registry/registry.h"

namespace donner::svg {

struct ComputedStyleComponent {
  ComputedStyleComponent() {}

  const PropertyRegistry& properties() const {
    assert(properties_);
    return properties_.value();
  }

  const Boxd& viewbox() const {
    assert(viewbox_.has_value());
    return viewbox_.value();
  }

  void computeProperties(EntityHandle handle);

private:
  std::optional<PropertyRegistry> properties_;
  std::optional<Boxd> viewbox_;
};

}  // namespace donner::svg
