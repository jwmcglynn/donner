#pragma once

namespace donner::svg {

enum class RenderingBehavior { Default, Nonrenderable, NoTraverseChildren };

struct RenderingBehaviorComponent {
  RenderingBehavior behavior = RenderingBehavior::Default;
};

}  // namespace donner::svg
