#pragma once
/// @file

#include "donner/base/Transform.h"
#include "donner/svg/core/CssTransform.h"
#include "donner/svg/properties/Property.h"

namespace donner::svg::components {

struct ComputedStyleComponent;

struct ComputedLocalTransformComponent {
  Transformd transform;
  CssTransform rawCssTransform;
};

struct TransformComponent {
  Property<CssTransform> transform{"transform",
                                   []() -> std::optional<CssTransform> { return std::nullopt; }};
};

}  // namespace donner::svg::components
