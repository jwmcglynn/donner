#pragma once
/// @file
/// Defines PreserveAspectRatio for SVG aspect ratio preservation.

#include <optional>
#include <ostream>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"

namespace donner::svg {

/// Handles SVG's preserveAspectRatio attribute.
class PreserveAspectRatio {
public:
  /// Alignment options for preserveAspectRatio.
  enum class Align : uint8_t {
    None,      ///< No forced uniform scaling
    XMinYMin,  ///< Left-top alignment
    XMidYMin,  ///< Center-top alignment
    XMaxYMin,  ///< Right-top alignment
    XMinYMid,  ///< Left-center alignment
    XMidYMid,  ///< Center-center alignment
    XMaxYMid,  ///< Right-center alignment
    XMinYMax,  ///< Left-bottom alignment
    XMidYMax,  ///< Center-bottom alignment
    XMaxYMax,  ///< Right-bottom alignment
  };

  /// Scaling methods for preserveAspectRatio.
  enum class MeetOrSlice : uint8_t {
    Meet,  ///< Scale to fit within viewport
    Slice  ///< Scale to cover entire viewport, clipping the content if necessary
  };

  /// Default: XMidYMid per SVG spec
  Align align = Align::XMidYMid;

  /// Default: Meet per SVG spec
  MeetOrSlice meetOrSlice = MeetOrSlice::Meet;

  /**
   * Creates a PreserveAspectRatio with 'none' alignment.
   * Useful for scenarios where aspect ratio should be ignored.
   */
  static PreserveAspectRatio None() { return PreserveAspectRatio{Align::None, MeetOrSlice::Meet}; }

  /**
   * Calculates the horizontal alignment factor.
   * @return 0 for left, 0.5 for center, 1 for right alignment.
   */
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

  /**
   * Calculates the vertical alignment factor.
   * @return 0 for top, 0.5 for middle, 1 for bottom alignment.
   */
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
   * Computes the transformation in destinationFromSource notation that maps coordinates from the
   * viewBox coordinate system (source) to the element's content coordinate system (destination).
   *
   * The `size` parameter represents the destination rectangle for the element's content, while the
   * optional `viewbox` parameter defines the source coordinate system. If no viewbox is provided,
   * the transformation is a simple translation to the top‐left of `size`.
   *
   * @see https://www.w3.org/TR/SVG2/coords.html#ComputingAViewportsTransform.
   *
   * @param size The destination rectangle for the element's content.
   * @param viewbox The source viewBox rectangle, if any.
   * @return The computed transformation mapping points from the viewBox (source) to the element's
   * content (destination).
   */
  Transformd elementContentFromViewBoxTransform(const Boxd& size,
                                                std::optional<Boxd> viewbox) const;

  /// Equality operator.
  bool operator==(const PreserveAspectRatio& other) const {
    return align == other.align && meetOrSlice == other.meetOrSlice;
  }

  /**
   * Outputs a string representation of PreserveAspectRatio to a stream.
   * Format: "PreserveAspectRatio {<align>, <meetOrSlice>}"
   * Example: "PreserveAspectRatio {Align::XMidYMid, MeetOrSlice::Meet}"
   */
  friend std::ostream& operator<<(std::ostream& os, const PreserveAspectRatio& value);
};

/// Outputs Align value to stream (e.g., "Align::XMidYMid")
std::ostream& operator<<(std::ostream& os, PreserveAspectRatio::Align value);

/// Outputs MeetOrSlice value to stream (e.g., "MeetOrSlice::Meet")
std::ostream& operator<<(std::ostream& os, PreserveAspectRatio::MeetOrSlice value);

}  // namespace donner::svg
