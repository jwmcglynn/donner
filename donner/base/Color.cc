#include "donner/base/Color.h"

#include <string>

namespace donner {

std::ostream& operator<<(std::ostream& os, const RGBA& color) {
  return os << "rgba(" << static_cast<int>(color.r) << ", " << static_cast<int>(color.g) << ", "
            << static_cast<int>(color.b) << ", " << static_cast<int>(color.a) << ")";
}

std::string RGBA::toHexString() const {
  // Convert this color to hex without using stringstream.
  constexpr char kHexDigits[] = "0123456789abcdef";

  std::string result = (a == 255) ? "#000000" : "#00000000";

  result[1] = kHexDigits[r >> 4];
  result[2] = kHexDigits[r & 0xf];
  result[3] = kHexDigits[g >> 4];
  result[4] = kHexDigits[g & 0xf];
  result[5] = kHexDigits[b >> 4];
  result[6] = kHexDigits[b & 0xf];
  if (a != 255) {
    result[7] = kHexDigits[a >> 4];
    result[8] = kHexDigits[a & 0xf];
  }

  return result;
}

}  // namespace donner
