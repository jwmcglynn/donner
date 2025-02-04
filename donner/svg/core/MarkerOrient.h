#pragma once
/// @file

#include "donner/base/MathUtils.h"
#include "donner/base/Vector2.h"

namespace donner::svg {

/**
 * Represents the orientation of a marker.
 */
struct MarkerOrient {
  /**
   * The type of the orientation.
   */
  enum class Type : uint8_t {
    Angle,             //!< Contains a user-provided angle.
    Auto,              //!< The angle is computed from the direction vector.
    AutoStartReverse,  //!< Like \ref Auto, but for `marker-start` the direction is reversed.
  };

  /// Default constructor.
  MarkerOrient() = default;

  /// Destructor.
  ~MarkerOrient() = default;

  /**
   * Creates a new orientation with a user-provided angle in radians.
   *
   * @param angleRadians The angle in radians.
   * @return The new orientation.
   */
  static MarkerOrient AngleRadians(double angleRadians) {
    MarkerOrient orient;
    orient.type_ = Type::Angle;
    orient.angleRadians_ = angleRadians;
    return orient;
  }

  /**
   * Creates a new orientation with a user-provided angle in radians.
   *
   * @param angleDegrees The angle in Degrees.
   * @return The new orientation.
   */
  static MarkerOrient AngleDegrees(double angleDegrees) {
    MarkerOrient orient;
    orient.type_ = Type::Angle;
    orient.angleRadians_ = angleDegrees * MathConstants<double>::kDegToRad;
    return orient;
  }

  /**
   * Creates a new orientation that computes the angle from the direction vector.
   *
   * @return The new orientation.
   */
  static MarkerOrient Auto() {
    MarkerOrient orient;
    orient.type_ = Type::Auto;
    return orient;
  }

  /**
   * Creates a new orientation that computes the angle from the direction vector, but reverses the
   * direction for `marker-start`.
   *
   * @return The new orientation.
   */
  static MarkerOrient AutoStartReverse() {
    MarkerOrient orient;
    orient.type_ = Type::AutoStartReverse;
    return orient;
  }

  // Copy and moveable.
  /// Copy constructor.
  MarkerOrient(const MarkerOrient&) = default;
  /// Move constructor.
  MarkerOrient(MarkerOrient&&) = default;
  /// Copy assignment operator.
  MarkerOrient& operator=(const MarkerOrient&) = default;
  /// Move assignment operator.
  MarkerOrient& operator=(MarkerOrient&&) = default;

  /// Returns the type of orientation.
  Type type() const { return type_; }

  /**
   * For \ref computeAngleRadians, to specify if this is the `marker-start` orientation.
   */
  enum class MarkerType : uint8_t {
    Default,  //!< The default orientation.
    Start,    //!< The `marker-start` orientation.
  };

  /**
   * Computes the angle in radians based on the direction vector and the type of orientation.
   *
   * @param direction The direction vector.
   * @param markerType Set to \ref MarkerType::Start if this is the `marker-start` orientation.
   * @return The angle in radians.
   */
  double computeAngleRadians(const Vector2d& direction,
                             MarkerType markerType = MarkerType::Default) const {
    if (type_ == Type::Angle) {
      return angleRadians_;
    }

    if (NearZero(direction.lengthSquared())) {
      return 0.0;
    }

    const double angle = std::atan2(direction.y, direction.x);
    if (type_ == Type::AutoStartReverse && markerType == MarkerType::Start) {
      return angle + MathConstants<double>::kPi;
    } else {
      return angle;
    }
  }

  bool operator==(const MarkerOrient& other) const {
    return type_ == other.type_ && NearEquals(angleRadians_, other.angleRadians_);
  }

private:
  Type type_ = Type::Angle;    //!< The type of orientation.
  double angleRadians_ = 0.0;  //!< The angle in radians, used if \ref type_ is \ref Type::Angle.
};

}  // namespace donner::svg
