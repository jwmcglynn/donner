#pragma once
/// @file

#include <limits>
#include <locale>
#include <sstream>

namespace donner {

/**
 * Returns an output string stream for serializing numbers that are read back later: it is imbued
 * with the classic "C" locale, so the C++ global locale cannot change the decimal point or add
 * digit grouping.
 *
 * @param precision Significant digits for floating-point output. The default round-trips doubles.
 */
inline std::ostringstream MakeClassicNumberStream(
    std::streamsize precision = std::numeric_limits<double>::max_digits10) {
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  stream.precision(precision);
  return stream;
}

}  // namespace donner
