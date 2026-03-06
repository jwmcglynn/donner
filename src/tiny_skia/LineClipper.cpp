#include "tiny_skia/LineClipper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace tiny_skia {

namespace {

constexpr float kNearlyZero = std::numeric_limits<float>::epsilon() * 4.0f;

bool isNearlyZero(float value) { return std::abs(value) <= kNearlyZero; }

float average(float a, float b) { return (a + b) * 0.5f; }

float pinUnsortedF32(float value, float limit0, float limit1) {
  if (limit1 < limit0) {
    std::swap(limit0, limit1);
  }

  if (value < limit0) {
    return limit0;
  }
  if (value > limit1) {
    return limit1;
  }
  return value;
}

double pinUnsortedF64(double value, double limit0, double limit1) {
  if (limit1 < limit0) {
    std::swap(limit0, limit1);
  }

  if (value < limit0) {
    return limit0;
  }
  if (value > limit1) {
    return limit1;
  }
  return value;
}

float sectWithHorizontal(const std::array<Point, 2>& src, float y) {
  const auto dy = src[1].y - src[0].y;
  if (isNearlyZero(dy)) {
    return average(src[0].x, src[1].x);
  }

  const auto x0 = static_cast<double>(src[0].x);
  const auto y0 = static_cast<double>(src[0].y);
  const auto x1 = static_cast<double>(src[1].x);
  const auto y1 = static_cast<double>(src[1].y);
  const auto result = x0 + (static_cast<double>(y) - y0) * (x1 - x0) / (y1 - y0);
  return static_cast<float>(pinUnsortedF64(result, x0, x1));
}

float sectWithVertical(const std::array<Point, 2>& src, float x) {
  const auto dx = src[1].x - src[0].x;
  if (isNearlyZero(dx)) {
    return average(src[0].y, src[1].y);
  }

  const auto x0 = static_cast<double>(src[0].x);
  const auto y0 = static_cast<double>(src[0].y);
  const auto x1 = static_cast<double>(src[1].x);
  const auto y1 = static_cast<double>(src[1].y);
  return static_cast<float>(y0 + (static_cast<double>(x) - x0) * (y1 - y0) / (x1 - x0));
}

bool containsNoEmptyCheck(const Rect& outer, const Rect& inner) {
  return outer.left() <= inner.left() && outer.top() <= inner.top() &&
         outer.right() >= inner.right() && outer.bottom() >= inner.bottom();
}

bool nestedLt(float a, float b, float dim) { return a <= b && (a < b || dim > 0.0f); }

}  // namespace

namespace lineClipper {

std::span<const Point> clip(std::span<const Point, 2> src, const Rect& clip, bool canCullToTheRight,
                            std::span<Point, kLineClipperMaxPoints> pointsOut) {
  int index0 = (src[0].y < src[1].y) ? 0 : 1;
  int index1 = (index0 == 0) ? 1 : 0;

  if (src[index1].y <= clip.top()) {
    return {};
  }
  if (src[index0].y >= clip.bottom()) {
    return {};
  }

  std::array<Point, 2> tmp = {src[0], src[1]};

  if (src[index0].y < clip.top()) {
    tmp[index0] = {sectWithHorizontal({src[0], src[1]}, clip.top()), clip.top()};
  }
  if (tmp[index1].y > clip.bottom()) {
    tmp[index1] = {sectWithHorizontal({src[0], src[1]}, clip.bottom()), clip.bottom()};
  }

  std::array<Point, kLineClipperMaxPoints> result = {Point{}, Point{}, Point{}, Point{}};
  int lineCount = 1;
  bool reverse = false;

  if (src[0].x < src[1].x) {
    index0 = 0;
    index1 = 1;
  } else {
    index0 = 1;
    index1 = 0;
    reverse = true;
  }

  const auto resultPoints = [&]() -> std::span<const Point> {
    if (tmp[index1].x <= clip.left()) {
      result[0] = {clip.left(), tmp[0].y};
      result[1] = {clip.left(), tmp[1].y};
      lineCount = 1;
      reverse = false;
      return std::span<const Point>(result.data(), lineCount + 1);
    }

    if (tmp[index0].x >= clip.right()) {
      if (canCullToTheRight) {
        return {};
      }
      result[0] = {clip.right(), tmp[0].y};
      result[1] = {clip.right(), tmp[1].y};
      lineCount = 1;
      reverse = false;
      return std::span<const Point>(result.data(), lineCount + 1);
    }

    int offset = 0;
    if (tmp[index0].x < clip.left()) {
      result[offset] = {clip.left(), tmp[index0].y};
      ++offset;
      result[offset] = {clip.left(), pinUnsortedF32(sectWithVertical(tmp, clip.left()),
                                                    tmp[index0].y, tmp[index1].y)};
    } else {
      result[offset] = tmp[index0];
    }

    ++offset;
    if (tmp[index1].x > clip.right()) {
      result[offset] = {clip.right(), pinUnsortedF32(sectWithVertical(tmp, clip.right()),
                                                     tmp[index0].y, tmp[index1].y)};
      ++offset;
      result[offset] = {clip.right(), tmp[index1].y};
    } else {
      result[offset] = tmp[index1];
    }

    lineCount = offset;
    return std::span<const Point>(result.data(), lineCount + 1);
  }();

  if (resultPoints.empty()) {
    return {};
  }

  if (reverse) {
    for (int i = 0; i <= lineCount; ++i) {
      pointsOut[lineCount - i] = resultPoints[i];
    }
  } else {
    std::copy(resultPoints.begin(), resultPoints.begin() + (lineCount + 1), pointsOut.begin());
  }

  return pointsOut.first(lineCount + 1);
}

bool intersect(std::span<const Point, 2> src, const Rect& clip, std::span<Point, 2> dst) {
  const auto left = std::min(src[0].x, src[1].x);
  const auto top = std::min(src[0].y, src[1].y);
  const auto right = std::max(src[0].x, src[1].x);
  const auto bottom = std::max(src[0].y, src[1].y);
  auto bounds = Rect::fromLTRB(left, top, right, bottom);
  if (bounds.has_value()) {
    const auto width = right - left;
    const auto height = bottom - top;
    if (containsNoEmptyCheck(clip, bounds.value())) {
      dst[0] = src[0];
      dst[1] = src[1];
      return true;
    }

    if (nestedLt(right, clip.left(), width) || nestedLt(clip.right(), left, width) ||
        nestedLt(bottom, clip.top(), height) || nestedLt(clip.bottom(), top, height)) {
      return false;
    }
  }

  int index0 = (src[0].y < src[1].y) ? 0 : 1;
  int index1 = (index0 == 0) ? 1 : 0;

  std::array<Point, 2> tmp = {src[0], src[1]};

  if (tmp[index0].y < clip.top()) {
    tmp[index0] = {sectWithHorizontal({src[0], src[1]}, clip.top()), clip.top()};
  }
  if (tmp[index1].y > clip.bottom()) {
    tmp[index1] = {sectWithHorizontal({src[0], src[1]}, clip.bottom()), clip.bottom()};
  }

  if (tmp[0].x < tmp[1].x) {
    index0 = 0;
    index1 = 1;
  } else {
    index0 = 1;
    index1 = 0;
  }

  if (tmp[index1].x <= clip.left() || tmp[index0].x >= clip.right()) {
    if (tmp[0].x != tmp[1].x || tmp[0].x < clip.left() || tmp[0].x > clip.right()) {
      return false;
    }
  }

  if (tmp[index0].x < clip.left()) {
    tmp[index0] = {clip.left(), sectWithVertical({src[0], src[1]}, clip.left())};
  }
  if (tmp[index1].x > clip.right()) {
    tmp[index1] = {clip.right(), sectWithVertical({src[0], src[1]}, clip.right())};
  }

  dst[0] = tmp[0];
  dst[1] = tmp[1];
  return true;
}

}  // namespace lineClipper
}  // namespace tiny_skia
