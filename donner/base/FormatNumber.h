#pragma once
/// @file

#include <cmath>
#include <cstdint>
#include <format>
#include <string>

namespace donner::detail {

/// Format a single `double` into the minimum-length round-trippable decimal form.
///
/// - Integer-valued finite doubles (e.g. `10.0`, `-3.0`) are cast to `int64_t` and
///   formatted without a decimal point or scientific notation.
/// - All other doubles use `std::format`'s `{}` default specifier, which per C++20
///   `[format.string.std]` emits the shortest decimal representation that round-trips
///   through `std::from_chars`.  The `{:g}` specifier was tried and rejected — it
///   defaults to 6-significant-digit precision, which drops accuracy for values like
///   `tan(π/6) = 0.57735026918962562`.
///
/// Used by \ref donner::toSVGTransformString "toSVGTransformString",
/// \ref donner::Path::toSVGPathData "Path::toSVGPathData", and
/// \ref donner::Length::toRcString "Length::toRcString()".
inline std::string FormatNumberForSVG(double value) {
  if (value == std::trunc(value) && std::isfinite(value)) {
    return std::format("{}", static_cast<std::int64_t>(value));
  }
  return std::format("{}", value);
}

}  // namespace donner::detail
