#include "donner/svg/components/ViewboxComponent.h"

namespace donner::svg::components {

Transformd ViewboxComponent::computeTransform(Boxd size,
                                              PreserveAspectRatio preserveAspectRatio) const {
  if (!viewbox) {
    return Transformd::Translate(size.topLeft);
  }

  Vector2d scale = size.size() / viewbox->size();
  if (preserveAspectRatio.align != PreserveAspectRatio::Align::None) {
    if (preserveAspectRatio.meetOrSlice == PreserveAspectRatio::MeetOrSlice::Meet) {
      scale.x = scale.y = std::min(scale.x, scale.y);
    } else {
      scale.x = scale.y = std::max(scale.x, scale.y);
    }
  }

  Vector2d translation = size.topLeft - (viewbox->topLeft * scale);
  const Vector2d alignMaxOffset = size.size() - viewbox->size() * scale;

  const Vector2d alignMultiplier(preserveAspectRatio.alignMultiplierX(),
                                 preserveAspectRatio.alignMultiplierY());
  return Transformd::Scale(scale) *
         Transformd::Translate(translation + alignMaxOffset * alignMultiplier);
}

}  // namespace donner::svg::components