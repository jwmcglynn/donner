#pragma once

namespace donner::svg {

enum class RenderingBehavior { Default, Nonrenderable };

struct RenderingBehaviorComponent {
  RenderingBehavior behavior = RenderingBehavior::Default;
};

}  // namespace donner::svg
