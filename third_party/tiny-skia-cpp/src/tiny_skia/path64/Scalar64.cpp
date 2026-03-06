#include "tiny_skia/path64/Scalar64.h"

namespace tiny_skia::path64 {

namespace scalar64 {

double cubeRoot(double value) {
  if (approximatelyZeroCubed(value)) {
    return 0.0;
  }
  return std::cbrt(value);
}

}  // namespace scalar64
}  // namespace tiny_skia::path64
