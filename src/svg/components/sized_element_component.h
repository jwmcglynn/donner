#pragma once

#include <optional>

#include "src/base/box.h"
#include "src/base/length.h"

namespace donner {

/**
 * Stores an offset/size for elements that are positioned with x/y/width/height attributes with
 * respect to their parent. Used for <svg>, <image> and <foreignObject>.
 *
 * If not specified, x/y default to 0, and width/height are std::nullopt.
 */
struct SizedElementComponent {
  Lengthd x;
  Lengthd y;
  std::optional<Lengthd> width;
  std::optional<Lengthd> height;
};

}  // namespace donner