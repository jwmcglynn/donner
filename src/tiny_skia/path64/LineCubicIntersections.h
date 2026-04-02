#pragma once

#include <array>

#include "tiny_skia/path64/Cubic64.h"

namespace tiny_skia::path64::line_cubic_intersections {

std::size_t horizontalIntersect(const cubic64::Cubic64& cubic, double axisIntercept,
                                std::array<double, 3>& roots);

std::size_t verticalIntersect(const cubic64::Cubic64& cubic, double axisIntercept,
                              std::array<double, 3>& roots);

}  // namespace tiny_skia::path64::line_cubic_intersections
