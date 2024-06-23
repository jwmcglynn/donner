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

std::ostream& operator<<(std::ostream& os, const PreserveAspectRatio& value) {
  return os << "PreserveAspectRatio {" << value.align << ", " << value.meetOrSlice << "}";
}

std::ostream& operator<<(std::ostream& os, PreserveAspectRatio::Align value) {
  switch (value) {
    case PreserveAspectRatio::Align::None: os << "Align::None"; break;
    case PreserveAspectRatio::Align::XMinYMin: os << "Align::XMinYMin"; break;
    case PreserveAspectRatio::Align::XMidYMin: os << "Align::XMidYMin"; break;
    case PreserveAspectRatio::Align::XMaxYMin: os << "Align::XMaxYMin"; break;
    case PreserveAspectRatio::Align::XMinYMid: os << "Align::XMinYMid"; break;
    case PreserveAspectRatio::Align::XMidYMid: os << "Align::XMidYMid"; break;
    case PreserveAspectRatio::Align::XMaxYMid: os << "Align::XMaxYMid"; break;
    case PreserveAspectRatio::Align::XMinYMax: os << "Align::XMinYMax"; break;
    case PreserveAspectRatio::Align::XMidYMax: os << "Align::XMidYMax"; break;
    case PreserveAspectRatio::Align::XMaxYMax: os << "Align::XMaxYMax"; break;
  }

  return os;
}

std::ostream& operator<<(std::ostream& os, PreserveAspectRatio::MeetOrSlice value) {
  switch (value) {
    case PreserveAspectRatio::MeetOrSlice::Meet: os << "MeetOrSlice::Meet"; break;
    case PreserveAspectRatio::MeetOrSlice::Slice: os << "MeetOrSlice::Slice"; break;
  }

  return os;
}

}  // namespace donner::svg
