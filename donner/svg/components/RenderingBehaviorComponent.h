#pragma once
/// @file

namespace donner::svg::components {

enum class RenderingBehavior { Default, Nonrenderable, NoTraverseChildren, ShadowOnlyChildren };

struct RenderingBehaviorComponent {
  RenderingBehavior behavior = RenderingBehavior::Default;
  bool appliesTransform = true;

  explicit RenderingBehaviorComponent(RenderingBehavior behavior) : behavior(behavior) {}
};

}  // namespace donner::svg::components