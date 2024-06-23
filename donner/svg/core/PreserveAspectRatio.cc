#include "donner/svg/core/PreserveAspectRatio.h"

#include <optional>

namespace donner::svg {

Transformd PreserveAspectRatio::computeTransform(const Boxd& size,
                                                 std::optional<Boxd> viewbox) const {
  if (!viewbox) {
    return Transformd::Translate(size.topLeft);
  }

  Vector2d scale = size.size() / viewbox->size();
  if (this->align != PreserveAspectRatio::Align::None) {
    if (this->meetOrSlice == PreserveAspectRatio::MeetOrSlice::Meet) {
      scale.x = scale.y = std::min(scale.x, scale.y);
    } else {
      scale.x = scale.y = std::max(scale.x, scale.y);
    }
  }

  Vector2d translation = size.topLeft - (viewbox->topLeft * scale);
  const Vector2d alignMaxOffset = size.size() - viewbox->size() * scale;

  const Vector2d alignMultiplier(alignMultiplierX(), alignMultiplierY());
  return Transformd::Scale(scale) *
         Transformd::Translate(translation + alignMaxOffset * alignMultiplier);
}

}  // namespace donner::svg
