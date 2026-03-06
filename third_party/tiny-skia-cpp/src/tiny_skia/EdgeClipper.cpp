#include "tiny_skia/EdgeClipper.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <span>

#include "tiny_skia/LineClipper.h"
#include "tiny_skia/PathGeometry.h"

namespace tiny_skia {

namespace {

constexpr float kCubicFloatLimit = static_cast<float>(1 << 22);

void chopMonoCubicAtYFallback(const std::array<Point, 4>& src, float y, std::array<Point, 7>& dst);

bool quickReject(const Rect& bounds, const Rect& clip) {
  return bounds.top() >= clip.bottom() || bounds.bottom() <= clip.top();
}

template <std::size_t N>
bool sortIncreasingY(const std::array<Point, N>& src, std::array<Point, N>& dst) {
  if (src[0].y > src[N - 1].y) {
    for (std::size_t i = 0; i < N; ++i) {
      dst[i] = src[N - 1 - i];
    }
    return true;
  }

  dst = src;
  return false;
}

template <std::size_t N>
std::optional<Rect> boundsFromPoints(const std::array<Point, N>& points) {
  auto left = points[0].x;
  auto top = points[0].y;
  auto right = points[0].x;
  auto bottom = points[0].y;

  for (const auto& point : points) {
    left = std::min(left, point.x);
    top = std::min(top, point.y);
    right = std::max(right, point.x);
    bottom = std::max(bottom, point.y);
  }

  return Rect::fromLTRB(left, top, right, bottom);
}

void chopQuadInY(const Rect& clip, std::array<Point, 3>& pts) {
  float t = 0.0f;
  auto tmp = std::array<Point, 5>{};

  if (pts[0].y < clip.top()) {
    if (pathGeometry::chopMonoQuadAtY(pts, clip.top(), t)) {
      pathGeometry::chopQuadAt(pts, t, tmp);
      tmp[2].y = clip.top();
      tmp[3].y = std::max(tmp[3].y, clip.top());
      pts[0] = tmp[2];
      pts[1] = tmp[3];
    } else {
      for (auto& point : pts) {
        if (point.y < clip.top()) {
          point.y = clip.top();
        }
      }
    }
  }

  if (pts[2].y > clip.bottom()) {
    if (pathGeometry::chopMonoQuadAtY(pts, clip.bottom(), t)) {
      pathGeometry::chopQuadAt(pts, t, tmp);
      tmp[1].y = std::min(tmp[1].y, clip.bottom());
      tmp[2].y = clip.bottom();
      pts[1] = tmp[1];
      pts[2] = tmp[2];
    } else {
      for (auto& point : pts) {
        if (point.y > clip.bottom()) {
          point.y = clip.bottom();
        }
      }
    }
  }
}

bool tooBigForReliableFloatMath(const Rect& bounds) {
  return bounds.left() < -kCubicFloatLimit || bounds.top() < -kCubicFloatLimit ||
         bounds.right() > kCubicFloatLimit || bounds.bottom() > kCubicFloatLimit;
}

void chopCubicInY(const Rect& clip, std::array<Point, 4>& pts) {
  auto tmp = std::array<Point, 7>{};

  if (pts[0].y < clip.top()) {
    chopMonoCubicAtYFallback(pts, clip.top(), tmp);
    if (tmp[3].y < clip.top() && tmp[4].y < clip.top() && tmp[5].y < clip.top()) {
      const auto recheck = std::array<Point, 4>{tmp[3], tmp[4], tmp[5], tmp[6]};
      chopMonoCubicAtYFallback(recheck, clip.top(), tmp);
    }

    tmp[3].y = clip.top();
    tmp[4].y = std::max(tmp[4].y, clip.top());
    pts[0] = tmp[3];
    pts[1] = tmp[4];
    pts[2] = tmp[5];
  }

  if (pts[3].y > clip.bottom()) {
    chopMonoCubicAtYFallback(pts, clip.bottom(), tmp);
    tmp[3].y = clip.bottom();
    tmp[2].y = std::min(tmp[2].y, clip.bottom());
    pts[1] = tmp[1];
    pts[2] = tmp[2];
    pts[3] = tmp[3];
  }
}

double monoCubicClosestT(std::array<float, 4> src, float target) {
  // Use float arithmetic to match Rust semantics — using double causes
  // the binary search to never converge when the target is outside the
  // curve's range, because double denormals extend much further than float.
  float t = 0.5f;
  float lastT = t;
  float bestT = t;
  float step = 0.25f;
  const float d = src[0];
  const float a = src[3] + 3.0f * (src[1] - src[2]) - d;
  const float b = 3.0f * (src[2] - src[1] - src[1] + d);
  const float c = 3.0f * (src[1] - d);
  float x = target - d;
  float closest = std::numeric_limits<float>::max();

  for (;;) {
    const float loc = ((a * t + b) * t + c) * t;
    const float dist = std::abs(loc - x);
    if (closest > dist) {
      closest = dist;
      bestT = t;
    }
    lastT = t;
    t += (loc < x) ? step : -step;
    step *= 0.5f;
    if (!(closest > 0.25f && lastT != t)) {
      break;
    }
  }

  if (bestT <= 0.0f) {
    return std::numeric_limits<double>::epsilon();
  }
  if (bestT >= 1.0f) {
    return 1.0 - std::numeric_limits<double>::epsilon();
  }
  return static_cast<double>(bestT);
}

void chopMonoCubicAtXFallback(const std::array<Point, 4>& src, float x, std::array<Point, 7>& dst) {
  if (pathGeometry::chopMonoCubicAtX(src, x, dst)) {
    return;
  }

  const auto t = NormalizedF32Exclusive::newBounded(static_cast<float>(
      monoCubicClosestT(std::array<float, 4>{src[0].x, src[1].x, src[2].x, src[3].x}, x)));
  pathGeometry::chopCubicAt2(src.data(), t, dst.data());
}

void chopMonoCubicAtYFallback(const std::array<Point, 4>& src, float y, std::array<Point, 7>& dst) {
  if (pathGeometry::chopMonoCubicAtY(src, y, dst)) {
    return;
  }

  const auto t = NormalizedF32Exclusive::newBounded(static_cast<float>(
      monoCubicClosestT(std::array<float, 4>{src[0].y, src[1].y, src[2].y, src[3].y}, y)));
  pathGeometry::chopCubicAt2(src.data(), t, dst.data());
}

}  // namespace

EdgeClipper::EdgeClipper(Rect clip, bool canCullToTheRight)
    : clip_(clip), canCullToTheRight_(canCullToTheRight) {}

std::optional<ClippedEdges> EdgeClipper::clipLine(Point p0, Point p1) {
  auto pointsOut = std::array<Point, lineClipper::kLineClipperMaxPoints>{};
  const auto src = std::array<Point, 2>{p0, p1};
  const auto clipped =
      lineClipper::clip(std::span<const Point, 2>(src), clip_, canCullToTheRight_,
                         std::span<Point, lineClipper::kLineClipperMaxPoints>(pointsOut));

  for (std::size_t i = 0; i < clipped.size(); ++i) {
    if (i + 1 >= clipped.size()) {
      break;
    }
    pushLine(clipped[i], clipped[i + 1]);
  }

  if (edges_.empty()) {
    return std::nullopt;
  }
  return edges_;
}

void EdgeClipper::pushLine(Point p0, Point p1) {
  [[maybe_unused]] const auto pushed = edges_.pushLine(p0, p1);
  assert(pushed);
}

void EdgeClipper::pushVerticalLine(float x, float y0, float y1, bool reverse) {
  if (reverse) {
    std::swap(y0, y1);
  }
  [[maybe_unused]] const auto pushed = edges_.pushLine(Point{x, y0}, Point{x, y1});
  assert(pushed);
}

std::optional<ClippedEdges> EdgeClipper::clipQuad(Point p0, Point p1, Point p2) {
  auto points = std::array<Point, 3>{p0, p1, p2};
  const auto bounds = boundsFromPoints(points);
  if (!bounds.has_value() || quickReject(bounds.value(), clip_)) {
    return std::nullopt;
  }

  auto monoY = std::array<Point, 5>{};
  const auto countY = pathGeometry::chopQuadAtYExtrema(points, monoY);
  for (std::size_t y = 0; y <= countY; ++y) {
    auto monoX = std::array<Point, 5>{};
    const auto yPoints = std::array<Point, 3>{monoY[y * 2], monoY[y * 2 + 1], monoY[y * 2 + 2]};
    const auto countX = pathGeometry::chopQuadAtXExtrema(yPoints, monoX);
    for (std::size_t x = 0; x <= countX; ++x) {
      const auto xPoints = std::array<Point, 3>{monoX[x * 2], monoX[x * 2 + 1], monoX[x * 2 + 2]};
      clipMonoQuad(xPoints);
    }
  }

  if (edges_.empty()) {
    return std::nullopt;
  }
  return edges_;
}

void EdgeClipper::clipMonoQuad(const std::array<Point, 3>& src) {
  auto pts = std::array<Point, 3>{};
  auto reverse = sortIncreasingY(src, pts);

  if (pts[2].y <= clip_.top() || pts[0].y >= clip_.bottom()) {
    return;
  }

  chopQuadInY(clip_, pts);

  if (pts[0].x > pts[2].x) {
    std::swap(pts[0], pts[2]);
    reverse = !reverse;
  }

  assert(pts[0].x <= pts[1].x);
  assert(pts[1].x <= pts[2].x);

  if (pts[2].x <= clip_.left()) {
    pushVerticalLine(clip_.left(), pts[0].y, pts[2].y, reverse);
    return;
  }

  if (pts[0].x >= clip_.right()) {
    if (!canCullToTheRight_) {
      pushVerticalLine(clip_.right(), pts[0].y, pts[2].y, reverse);
    }
    return;
  }

  auto t = 0.0f;
  auto tmp = std::array<Point, 5>{};
  if (pts[0].x < clip_.left()) {
    if (pathGeometry::chopMonoQuadAtX(pts, clip_.left(), t)) {
      pathGeometry::chopQuadAt(pts, t, tmp);
      pushVerticalLine(clip_.left(), tmp[0].y, tmp[2].y, reverse);
      tmp[2].x = clip_.left();
      tmp[3].x = std::max(tmp[3].x, clip_.left());

      pts[0] = tmp[2];
      pts[1] = tmp[3];
    } else {
      pushVerticalLine(clip_.left(), pts[0].y, pts[2].y, reverse);
      return;
    }
  }

  if (pts[2].x > clip_.right()) {
    if (pathGeometry::chopMonoQuadAtX(pts, clip_.right(), t)) {
      pathGeometry::chopQuadAt(pts, t, tmp);
      tmp[1].x = std::min(tmp[1].x, clip_.right());
      tmp[2].x = clip_.right();

      pushQuad(std::array<Point, 3>{tmp[0], tmp[1], tmp[2]}, reverse);
      pushVerticalLine(clip_.right(), tmp[2].y, tmp[4].y, reverse);
    } else {
      pts[1].x = std::min(pts[1].x, clip_.right());
      pts[2].x = std::min(pts[2].x, clip_.right());
      pushQuad(pts, reverse);
    }
  } else {
    pushQuad(pts, reverse);
  }
}

void EdgeClipper::pushQuad(const std::array<Point, 3>& pts, bool reverse) {
  [[maybe_unused]] const auto pushed = edges_.pushQuad(pts, reverse);
  assert(pushed);
}

std::optional<ClippedEdges> EdgeClipper::clipCubic(Point p0, Point p1, Point p2, Point p3) {
  const auto points = std::array<Point, 4>{p0, p1, p2, p3};
  const auto bounds = boundsFromPoints(points);
  if (!bounds.has_value() || bounds->bottom() <= clip_.top() || bounds->top() >= clip_.bottom()) {
    return std::nullopt;
  }

  if (tooBigForReliableFloatMath(bounds.value())) {
    return clipLine(points[0], points[3]);
  }

  auto monoY = std::array<Point, 10>{};
  const auto countY = pathGeometry::chopCubicAtYExtrema(points, monoY);
  for (std::size_t y = 0; y <= countY; ++y) {
    auto monoX = std::array<Point, 10>{};
    const auto yPoints =
        std::array<Point, 4>{monoY[y * 3], monoY[y * 3 + 1], monoY[y * 3 + 2], monoY[y * 3 + 3]};
    const auto countX = pathGeometry::chopCubicAtXExtrema(yPoints, monoX);
    for (std::size_t x = 0; x <= countX; ++x) {
      const auto xPoints =
          std::array<Point, 4>{monoX[x * 3], monoX[x * 3 + 1], monoX[x * 3 + 2], monoX[x * 3 + 3]};
      clipMonoCubic(xPoints);
    }
  }

  if (edges_.empty()) {
    return std::nullopt;
  }
  return edges_;
}

void EdgeClipper::clipMonoCubic(const std::array<Point, 4>& src) {
  auto pts = std::array<Point, 4>{};
  auto reverse = sortIncreasingY(src, pts);

  if (pts[3].y <= clip_.top() || pts[0].y >= clip_.bottom()) {
    return;
  }

  chopCubicInY(clip_, pts);

  if (pts[0].x > pts[3].x) {
    std::swap(pts[0], pts[3]);
    std::swap(pts[1], pts[2]);
    reverse = !reverse;
  }

  if (pts[3].x <= clip_.left()) {
    pushVerticalLine(clip_.left(), pts[0].y, pts[3].y, reverse);
    return;
  }

  if (pts[0].x >= clip_.right()) {
    if (!canCullToTheRight_) {
      pushVerticalLine(clip_.right(), pts[0].y, pts[3].y, reverse);
    }
    return;
  }

  if (pts[0].x < clip_.left()) {
    auto tmp = std::array<Point, 7>{};
    chopMonoCubicAtXFallback(pts, clip_.left(), tmp);
    pushVerticalLine(clip_.left(), tmp[0].y, tmp[3].y, reverse);
    tmp[3].x = clip_.left();
    tmp[4].x = std::max(tmp[4].x, clip_.left());

    pts[0] = tmp[3];
    pts[1] = tmp[4];
    pts[2] = tmp[5];
  }

  if (pts[3].x > clip_.right()) {
    auto tmp = std::array<Point, 7>{};
    chopMonoCubicAtXFallback(pts, clip_.right(), tmp);
    tmp[3].x = clip_.right();
    tmp[2].x = std::min(tmp[2].x, clip_.right());

    pushCubic(std::array<Point, 4>{tmp[0], tmp[1], tmp[2], tmp[3]}, reverse);
    pushVerticalLine(clip_.right(), tmp[3].y, tmp[6].y, reverse);
  } else {
    pushCubic(pts, reverse);
  }
}

void EdgeClipper::pushCubic(const std::array<Point, 4>& pts, bool reverse) {
  [[maybe_unused]] const auto pushed = edges_.pushCubic(pts, reverse);
  assert(pushed);
}

EdgeClipperIter::EdgeClipperIter(const Path& path, Rect clip, bool canCullToTheRight)
    : edgeIter_(pathIter(path)), clip_(clip), canCullToTheRight_(canCullToTheRight) {}

std::optional<ClippedEdges> EdgeClipperIter::next() {
  for (auto edgeOpt = edgeIter_.next(); edgeOpt.has_value(); edgeOpt = edgeIter_.next()) {
    const auto& edge = edgeOpt.value();
    EdgeClipper clipper(clip_, canCullToTheRight_);
    if (edge.type == PathEdgeType::LineTo) {
      if (const auto edges = clipper.clipLine(edge.points[0], edge.points[1])) {
        return edges;
      }
    } else if (edge.type == PathEdgeType::QuadTo) {
      if (const auto edges = clipper.clipQuad(edge.points[0], edge.points[1], edge.points[2])) {
        return edges;
      }
    } else {
      if (const auto edges =
              clipper.clipCubic(edge.points[0], edge.points[1], edge.points[2], edge.points[3])) {
        return edges;
      }
    }
  }
  return std::nullopt;
}

}  // namespace tiny_skia
