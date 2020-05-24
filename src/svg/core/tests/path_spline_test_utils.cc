#include "src/svg/core/tests/path_spline_test_utils.h"

#include <gtest/gtest.h>

#include "src/base/utils.h"

namespace donner {

bool operator==(const PathSpline::Command& lhs, const PathSpline::Command& rhs) {
  return lhs.point_index == rhs.point_index && lhs.type == rhs.type;
}

std::ostream& operator<<(std::ostream& os, PathSpline::CommandType type) {
  switch (type) {
    case PathSpline::CommandType::MoveTo: os << "CommandType::MoveTo"; break;
    case PathSpline::CommandType::LineTo: os << "CommandType::LineTo"; break;
    case PathSpline::CommandType::CurveTo: os << "CommandType::CurveTo"; break;
    case PathSpline::CommandType::ClosePath: os << "CommandType::ClosePath"; break;
    default: UTILS_RELEASE_ASSERT(false && "Invalid command");
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const PathSpline::Command& command) {
  os << "Command {" << command.type << ", " << command.point_index << "}";
  return os;
}

void PrintTo(const PathSpline& spline, std::ostream* os) {
  *os << "PathSpline {";
  *os << " points: " << testing::PrintToString(spline.points());
  *os << " commands: " << testing::PrintToString(spline.commands());
  *os << " }";
}

}  // namespace donner
