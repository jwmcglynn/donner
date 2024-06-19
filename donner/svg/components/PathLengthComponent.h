#pragma once
/// @file

namespace donner::svg::components {

/**
 * Stores a user-overridden path length on an element. If the user has not overridden the path
 * length, this component will not be attached.
 */
struct PathLengthComponent {
  double value;  ///< The user-overridden path length, in user units.
};

}  // namespace donner::svg::components
