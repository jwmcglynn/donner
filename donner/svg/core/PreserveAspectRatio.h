#pragma once
/// @file

#include <optional>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"

namespace donner::svg {

class PreserveAspectRatio {
public:
  enum class Align {
    None,
    XMinYMin,
    XMidYMin,
    XMaxYMin,
    XMinYMid,
    XMidYMid,
    XMaxYMid,
    XMinYMax,
    XMidYMax,
    XMaxYMax,
  };

  enum class MeetOrSlice { Meet, Slice };

  // Defaults per https://www.w3.org/TR/SVG2/coords.html#ViewBoxAttribute
  Align align = Align::XMidYMid;
  MeetOrSlice meetOrSlice = MeetOrSlice::Meet;

  static PreserveAspectRatio None() { return PreserveAspectRatio{Align::None, MeetOrSlice::Meet}; }

  double alignMultiplierX() const {
    switch (align) {
      case Align::XMidYMin:
      case Align::XMidYMid:
      case Align::XMidYMax: return 0.5;
      case Align::XMaxYMin:
      case Align::XMaxYMid:
      case Align::XMaxYMax: return 1.0;
      default: return 0.0;
    }
  }

  double alignMultiplierY() const {
    switch (align) {
      case Align::XMinYMid:
      case Align::XMidYMid:
      case Align::XMaxYMid: return 0.5;
      case Align::XMinYMax:
      case Align::XMidYMax:
      case Align::XMaxYMax: return 1.0;
      default: return 0.0;
    }
  }

  /**
   * Computes the transform for the given Viewbox per
   * https://www.w3.org/TR/SVG2/coords.html#ComputingAViewportsTransform
   *
   * @param size The position and size of the element.
   * @param viewbox The viewbox of the element, or nullopt if the element has no viewbox.
   */
  Transformd computeTransform(const Boxd& size, std::optional<Boxd> viewbox) const;

  bool operator==(const PreserveAspectRatio& other) const {
    return align == other.align && meetOrSlice == other.meetOrSlice;
  }
};

}  // namespace donner::svg
