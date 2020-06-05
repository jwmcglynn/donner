#pragma once

namespace donner {

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

  bool operator==(const PreserveAspectRatio& other) const {
    return align == other.align && meetOrSlice == other.meetOrSlice;
  }
};

}  // namespace donner
