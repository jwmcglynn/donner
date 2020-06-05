#pragma once

#include "src/base/box.h"
#include "src/base/length.h"

namespace donner {

/**
 * Stores an offset/size for elements that are positioned with x/y/width/height attributes with
 * respect to their parent. Used for <svg>, <image> and <foreignObject>.
 */
struct SizedElementComponent {
  Lengthd x;
  Lengthd y;
  Lengthd width;
  Lengthd height;
};

}  // namespace donner