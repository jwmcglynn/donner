#pragma once
/// @file

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
/// Defined out-of-line in FormatNumber.cc: this header is included nearly everywhere
/// (via Length.h and Transform.h), and an inline definition instantiates the
/// `std::format` machinery in every including translation unit.
///
/// Used by \ref donner::toSVGTransformString "toSVGTransformString",
/// \ref donner::Path::toSVGPathData "Path::toSVGPathData", and
/// \ref donner::Length::toRcString "Length::toRcString()".
std::string FormatNumberForSVG(double value);

}  // namespace donner::detail
