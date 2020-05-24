#pragma once

#include <ostream>

#include "src/svg/core/path_spline.h"

namespace donner {

bool operator==(const PathSpline::Command& lhs, const PathSpline::Command& rhs);

std::ostream& operator<<(std::ostream& os, PathSpline::CommandType type);
std::ostream& operator<<(std::ostream& os, const PathSpline::Command& command);

void PrintTo(const PathSpline& spline, std::ostream* os);

}  // namespace donner
