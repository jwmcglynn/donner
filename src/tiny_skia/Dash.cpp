// Copyright 2014 Google Inc.
// Copyright 2020 Yevhenii Reizner
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This module is a C++ port of SkDashPath, SkDashPathEffect, SkContourMeasure
// and SkPathMeasure from the Rust tiny-skia dash.rs.

#include "tiny_skia/Dash.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <utility>

#include "tiny_skia/PathGeometry.h"

namespace tiny_skia {

namespace {

constexpr std::uint32_t kMaxTValue = 0x3FFFFFFF;

// 1/kMaxTValue can't be represented exactly as a float, but it's close enough.
constexpr float kMaxTReciprocal = 1.0f / static_cast<float>(kMaxTValue);

/// Linearly interpolate between a and b using a NormalizedF32 t.
float interp(float a, float b, NormalizedF32 t) { return a + (b - a) * t.get(); }

/// Linearly interpolate between a and b using a raw float t in [0,1].
float interpSafe(float a, float b, float t) {
  assert(t >= 0.0f && t <= 1.0f);
  return a + (b - a) * t;
}

/// Check whether the t-span is large enough to warrant further subdivision.
std::uint32_t tSpanBigEnough(std::uint32_t tSpan) {
  assert(tSpan <= kMaxTValue);
  return tSpan >> 10;
}

/// Determine whether a quadratic is too curvy to be approximated by a line.
bool quadTooCurvy(Point p0, Point p1, Point p2, float tolerance) {
  // diff = (a/4 + b/2 + c/4) - (a/2 + c/2)
  // diff = -a/4 + b/2 - c/4
  float dx = p1.x * 0.5f - (p0.x + p2.x) * 0.5f * 0.5f;
  float dy = p1.y * 0.5f - (p0.y + p2.y) * 0.5f * 0.5f;

  float dist = std::max(std::abs(dx), std::abs(dy));
  return dist > tolerance;
}

/// Determine whether a cubic is too curvy for a chord approximation.
bool cheapDistExceedsLimit(Point pt, float x, float y, float tolerance) {
  float dist = std::max(std::abs(x - pt.x), std::abs(y - pt.y));
  return dist > tolerance;
}

bool cubicTooCurvy(Point p0, Point p1, Point p2, Point p3, float tolerance) {
  bool n0 = cheapDistExceedsLimit(p1, interpSafe(p0.x, p3.x, 1.0f / 3.0f),
                                  interpSafe(p0.y, p3.y, 1.0f / 3.0f), tolerance);

  bool n1 = cheapDistExceedsLimit(p2, interpSafe(p0.x, p3.x, 2.0f / 3.0f),
                                  interpSafe(p0.y, p3.y, 2.0f / 3.0f), tolerance);

  return n0 || n1;
}

/// Binary search for the segment whose cumulative distance contains `key`.
/// Returns a signed index: non-negative if exact match, bitwise-NOT of
/// insertion point otherwise (matching the Rust find_segment semantics).
std::int32_t findSegment(const std::vector<Segment>& base, float key) {
  std::uint32_t lo = 0;
  std::uint32_t hi = static_cast<std::uint32_t>(base.size() - 1);

  while (lo < hi) {
    std::uint32_t mid = (hi + lo) >> 1;
    if (base[mid].distance < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }

  if (base[hi].distance < key) {
    hi += 1;
    hi = ~hi;
  } else if (key < base[hi].distance) {
    hi = ~hi;
  }

  return static_cast<std::int32_t>(hi);
}

/// Compute position and/or tangent at parameter t on a measured segment.
void computePosTan(const Point* points, SegmentType segKind, NormalizedF32 t, Point* pos,
                   Point* tangent) {
  switch (segKind) {
    case SegmentType::Line: {
      if (pos != nullptr) {
        *pos =
            Point::fromXY(interp(points[0].x, points[1].x, t), interp(points[0].y, points[1].y, t));
      }
      if (tangent != nullptr) {
        tangent->setNormalize(points[1].x - points[0].x, points[1].y - points[0].y);
      }
      break;
    }
    case SegmentType::Quad: {
      if (pos != nullptr) {
        *pos = pathGeometry::evalQuadAt(points, t);
      }
      if (tangent != nullptr) {
        *tangent = pathGeometry::evalQuadTangentAt(points, t);
        tangent->normalize();
      }
      break;
    }
    case SegmentType::Cubic: {
      if (pos != nullptr) {
        *pos = pathGeometry::evalCubicPosAt(points, t);
      }
      if (tangent != nullptr) {
        *tangent = pathGeometry::evalCubicTangentAt(points, t);
        tangent->normalize();
      }
      break;
    }
  }
}

/// Emit a portion of a segment [startT, stopT] into the path builder.
void segmentTo(const Point* points, SegmentType segKind, NormalizedF32 startT, NormalizedF32 stopT,
               PathBuilder& pb) {
  assert(startT <= stopT);

  if (startT == stopT) {
    if (auto pt = pb.lastPoint()) {
      // Zero-length "on" segment: emit a zero-length line so the stroker
      // can produce end caps if appropriate.
      pb.lineTo(pt->x, pt->y);
    }
    return;
  }

  switch (segKind) {
    case SegmentType::Line: {
      if (stopT == NormalizedF32::ONE) {
        pb.lineTo(points[1].x, points[1].y);
      } else {
        pb.lineTo(interp(points[0].x, points[1].x, stopT), interp(points[0].y, points[1].y, stopT));
      }
      break;
    }
    case SegmentType::Quad: {
      Point tmp0[5];
      Point tmp1[5];
      if (startT == NormalizedF32::ZERO) {
        if (stopT == NormalizedF32::ONE) {
          pb.quadToPt(points[1], points[2]);
        } else {
          auto stopTe = NormalizedF32Exclusive::newBounded(stopT.get());
          pathGeometry::chopQuadAtT(points, stopTe, tmp0);
          pb.quadToPt(tmp0[1], tmp0[2]);
        }
      } else {
        auto startTe = NormalizedF32Exclusive::newBounded(startT.get());
        pathGeometry::chopQuadAtT(points, startTe, tmp0);
        if (stopT == NormalizedF32::ONE) {
          pb.quadToPt(tmp0[3], tmp0[4]);
        } else {
          float newT = (stopT.get() - startT.get()) / (1.0f - startT.get());
          auto newTe = NormalizedF32Exclusive::newBounded(newT);
          pathGeometry::chopQuadAtT(&tmp0[2], newTe, tmp1);
          pb.quadToPt(tmp1[1], tmp1[2]);
        }
      }
      break;
    }
    case SegmentType::Cubic: {
      Point tmp0[7];
      Point tmp1[7];
      if (startT == NormalizedF32::ZERO) {
        if (stopT == NormalizedF32::ONE) {
          pb.cubicToPt(points[1], points[2], points[3]);
        } else {
          auto stopTe = NormalizedF32Exclusive::newBounded(stopT.get());
          pathGeometry::chopCubicAt2(points, stopTe, tmp0);
          pb.cubicToPt(tmp0[1], tmp0[2], tmp0[3]);
        }
      } else {
        auto startTe = NormalizedF32Exclusive::newBounded(startT.get());
        pathGeometry::chopCubicAt2(points, startTe, tmp0);
        if (stopT == NormalizedF32::ONE) {
          pb.cubicToPt(tmp0[4], tmp0[5], tmp0[6]);
        } else {
          float newT = (stopT.get() - startT.get()) / (1.0f - startT.get());
          auto newTe = NormalizedF32Exclusive::newBounded(newT);
          pathGeometry::chopCubicAt2(&tmp0[3], newTe, tmp1);
          pb.cubicToPt(tmp1[1], tmp1[2], tmp1[3]);
        }
      }
      break;
    }
  }
}

/// Adjust dash offset to be in [0, len).
float adjustDashOffset(float offset, float len) {
  if (offset < 0.0f) {
    offset = -offset;
    if (offset > len) {
      offset = std::fmod(offset, len);
    }
    offset = len - offset;

    // Due to finite precision, it's possible that offset == len,
    // even after the subtract (if len >>> offset), so fix that here.
    assert(offset <= len);
    if (offset == len) {
      offset = 0.0f;
    }
    return offset;
  } else if (offset >= len) {
    return std::fmod(offset, len);
  } else {
    return offset;
  }
}

/// Find the first dash interval and remaining length after consuming the
/// offset.
std::pair<float, std::size_t> findFirstInterval(const std::vector<float>& dashArray,
                                                float dashOffset) {
  for (std::size_t i = 0; i < dashArray.size(); ++i) {
    float gap = dashArray[i];
    if (dashOffset > gap || (dashOffset == gap && gap != 0.0f)) {
      dashOffset -= gap;
    } else {
      return {gap - dashOffset, i};
    }
  }

  // If we get here, offset "appears" to be larger than our length. This
  // shouldn't happen with perfect precision, but we can accumulate errors
  // during the initial length computation (rounding can make our sum be too
  // big or too small). In that event, we just eat the error here.
  return {dashArray[0], 0};
}

bool isEven(std::size_t x) { return (x % 2) == 0; }

}  // namespace

// ---------------------------------------------------------------------------
// Segment
// ---------------------------------------------------------------------------

float Segment::scalarT() const {
  assert(tValue <= kMaxTValue);
  return static_cast<float>(tValue) * kMaxTReciprocal;
}

// ---------------------------------------------------------------------------
// DashParams
// ---------------------------------------------------------------------------

std::optional<DashParams> DashParams::create(const StrokeDash& dash) {
  if (dash.array.size() < 2 || dash.array.size() % 2 != 0) {
    return std::nullopt;
  }

  for (float v : dash.array) {
    if (v < 0.0f || !std::isfinite(v)) {
      return std::nullopt;
    }
  }

  float intervalLen = std::accumulate(dash.array.begin(), dash.array.end(), 0.0f);
  auto intervalLenChecked = NonZeroPositiveF32::create(intervalLen);
  if (!intervalLenChecked.has_value()) {
    return std::nullopt;
  }

  float adjustedOffset = adjustDashOffset(dash.offset, intervalLenChecked->get());
  auto [firstLen, firstIndex] = findFirstInterval(dash.array, adjustedOffset);

  DashParams params;
  params.intervalLen = intervalLenChecked->get();
  params.firstLen = firstLen;
  params.firstIndex = firstIndex;
  params.adjustedOffset = adjustedOffset;
  return params;
}

// ---------------------------------------------------------------------------
// ContourMeasure
// ---------------------------------------------------------------------------

void ContourMeasure::pushSegment(float startD, float stopD, bool startWithMoveTo,
                                 PathBuilder& pb) const {
  if (startD < 0.0f) {
    startD = 0.0f;
  }
  if (stopD > length) {
    stopD = length;
  }

  if (!(startD <= stopD)) {
    // catch NaN values as well
    return;
  }

  if (segments.empty()) {
    return;
  }

  auto startResult = distanceToSegment(startD);
  if (!startResult.has_value()) return;
  auto [segIndex, startT] = *startResult;
  Segment seg = segments[segIndex];

  auto stopResult = distanceToSegment(stopD);
  if (!stopResult.has_value()) return;
  auto [stopSegIndex, stopT] = *stopResult;
  Segment stopSeg = segments[stopSegIndex];

  assert(segIndex <= stopSegIndex);

  Point p = Point::zero();
  if (startWithMoveTo) {
    computePosTan(&points[seg.pointIndex], seg.kind, startT, &p, nullptr);
    pb.moveTo(p.x, p.y);
  }

  if (seg.pointIndex == stopSeg.pointIndex) {
    segmentTo(&points[seg.pointIndex], seg.kind, startT, stopT, pb);
  } else {
    std::size_t newSegIndex = segIndex;
    for (;;) {
      segmentTo(&points[seg.pointIndex], seg.kind, startT, NormalizedF32::ONE, pb);

      std::size_t oldPointIndex = seg.pointIndex;
      for (;;) {
        newSegIndex += 1;
        if (segments[newSegIndex].pointIndex != oldPointIndex) {
          break;
        }
      }
      seg = segments[newSegIndex];

      startT = NormalizedF32::ZERO;

      if (seg.pointIndex >= stopSeg.pointIndex) {
        break;
      }
    }

    segmentTo(&points[seg.pointIndex], seg.kind, NormalizedF32::ZERO, stopT, pb);
  }
}

std::optional<std::pair<std::size_t, NormalizedF32>> ContourMeasure::distanceToSegment(
    float distance) const {
  assert(distance >= 0.0f && distance <= length);

  std::int32_t index = findSegment(segments, distance);
  // don't care if we hit an exact match or not, so we xor index if negative
  index ^= index >> 31;
  auto uindex = static_cast<std::size_t>(index);
  Segment seg = segments[uindex];

  // now interpolate t-values with the prev segment (if possible)
  float startT = 0.0f;
  float startD = 0.0f;
  // check if the prev segment is legal, and references the same set of points
  if (uindex > 0) {
    startD = segments[uindex - 1].distance;
    if (segments[uindex - 1].pointIndex == seg.pointIndex) {
      assert(segments[uindex - 1].kind == seg.kind);
      startT = segments[uindex - 1].scalarT();
    }
  }

  assert(seg.scalarT() > startT);
  assert(distance >= startD);
  assert(seg.distance > startD);

  float t = startT + (seg.scalarT() - startT) * (distance - startD) / (seg.distance - startD);
  auto tNorm = NormalizedF32::create(t);
  if (!tNorm.has_value()) {
    return std::nullopt;
  }
  return std::make_pair(uindex, *tNorm);
}

float ContourMeasure::computeLineSeg(Point p0, Point p1, float distance, std::size_t pointIndex) {
  float d = p0.distance(p1);
  assert(d >= 0.0f);
  float prevD = distance;
  distance += d;
  if (distance > prevD) {
    assert(pointIndex < points.size());
    Segment seg;
    seg.distance = distance;
    seg.pointIndex = pointIndex;
    seg.tValue = kMaxTValue;
    seg.kind = SegmentType::Line;
    segments.push_back(seg);
  }
  return distance;
}

float ContourMeasure::computeQuadSegs(Point p0, Point p1, Point p2, float distance,
                                      std::uint32_t minT, std::uint32_t maxT,
                                      std::size_t pointIndex, float tolerance) {
  if (tSpanBigEnough(maxT - minT) != 0 && quadTooCurvy(p0, p1, p2, tolerance)) {
    Point tmp[5];
    std::uint32_t halfT = (minT + maxT) >> 1;

    Point srcPts[3] = {p0, p1, p2};
    pathGeometry::chopQuadAtT(srcPts, NormalizedF32Exclusive::HALF, tmp);
    distance =
        computeQuadSegs(tmp[0], tmp[1], tmp[2], distance, minT, halfT, pointIndex, tolerance);
    distance =
        computeQuadSegs(tmp[2], tmp[3], tmp[4], distance, halfT, maxT, pointIndex, tolerance);
  } else {
    float d = p0.distance(p2);
    float prevD = distance;
    distance += d;
    if (distance > prevD) {
      assert(pointIndex < points.size());
      Segment seg;
      seg.distance = distance;
      seg.pointIndex = pointIndex;
      seg.tValue = maxT;
      seg.kind = SegmentType::Quad;
      segments.push_back(seg);
    }
  }
  return distance;
}

float ContourMeasure::computeCubicSegs(Point p0, Point p1, Point p2, Point p3, float distance,
                                       std::uint32_t minT, std::uint32_t maxT,
                                       std::size_t pointIndex, float tolerance) {
  if (tSpanBigEnough(maxT - minT) != 0 && cubicTooCurvy(p0, p1, p2, p3, tolerance)) {
    Point tmp[7];
    std::uint32_t halfT = (minT + maxT) >> 1;

    Point srcPts[4] = {p0, p1, p2, p3};
    pathGeometry::chopCubicAt2(srcPts, NormalizedF32Exclusive::HALF, tmp);
    distance = computeCubicSegs(tmp[0], tmp[1], tmp[2], tmp[3], distance, minT, halfT, pointIndex,
                                tolerance);
    distance = computeCubicSegs(tmp[3], tmp[4], tmp[5], tmp[6], distance, halfT, maxT, pointIndex,
                                tolerance);
  } else {
    float d = p0.distance(p3);
    float prevD = distance;
    distance += d;
    if (distance > prevD) {
      assert(pointIndex < points.size());
      Segment seg;
      seg.distance = distance;
      seg.pointIndex = pointIndex;
      seg.tValue = maxT;
      seg.kind = SegmentType::Cubic;
      segments.push_back(seg);
    }
  }
  return distance;
}

// ---------------------------------------------------------------------------
// ContourMeasureIter
// ---------------------------------------------------------------------------

ContourMeasureIter::ContourMeasureIter(const Path& path, float resScale) : iter_(path) {
  // can't use tangents, since we need [0..1..................2] to be seen
  // as definitely not a line (it is when drawn, but not parametrically)
  // so we compare midpoints
  constexpr float kCheapDistLimit = 0.5f;  // just made this value up

  float invResScale = (resScale != 0.0f) ? (1.0f / resScale) : 1.0f;
  tolerance_ = kCheapDistLimit * invResScale;
}

std::optional<ContourMeasure> ContourMeasureIter::next() {
  // Note:
  // as we accumulate distance, we have to check that the result of +=
  // actually made it larger, since a very small delta might be > 0, but
  // still have no effect on distance (if distance >>> delta).
  //
  // We do this check below, and in computeQuadSegs and computeCubicSegs

  ContourMeasure contour;

  std::size_t pointIndex = 0;
  float distance = 0.0f;
  bool haveSeenClose = false;
  Point prevP = Point::zero();

  while (auto seg = iter_.next()) {
    switch (seg->kind) {
      case PathSegment::Kind::MoveTo: {
        contour.points.push_back(seg->pts[0]);
        prevP = seg->pts[0];
        break;
      }
      case PathSegment::Kind::LineTo: {
        float prevD = distance;
        distance = contour.computeLineSeg(prevP, seg->pts[0], distance, pointIndex);
        if (distance > prevD) {
          contour.points.push_back(seg->pts[0]);
          pointIndex += 1;
        }
        prevP = seg->pts[0];
        break;
      }
      case PathSegment::Kind::QuadTo: {
        float prevD = distance;
        distance = contour.computeQuadSegs(prevP, seg->pts[0], seg->pts[1], distance, 0, kMaxTValue,
                                           pointIndex, tolerance_);
        if (distance > prevD) {
          contour.points.push_back(seg->pts[0]);
          contour.points.push_back(seg->pts[1]);
          pointIndex += 2;
        }
        prevP = seg->pts[1];
        break;
      }
      case PathSegment::Kind::CubicTo: {
        float prevD = distance;
        distance = contour.computeCubicSegs(prevP, seg->pts[0], seg->pts[1], seg->pts[2], distance,
                                            0, kMaxTValue, pointIndex, tolerance_);
        if (distance > prevD) {
          contour.points.push_back(seg->pts[0]);
          contour.points.push_back(seg->pts[1]);
          contour.points.push_back(seg->pts[2]);
          pointIndex += 3;
        }
        prevP = seg->pts[2];
        break;
      }
      case PathSegment::Kind::Close: {
        haveSeenClose = true;
        break;
      }
    }

    // If the next verb is a MoveTo, we are done with this contour.
    if (iter_.nextVerb() == PathVerb::Move) {
      break;
    }
  }

  if (!std::isfinite(distance)) {
    return std::nullopt;
  }

  if (haveSeenClose) {
    float prevD = distance;
    Point firstPt = contour.points[0];
    distance = contour.computeLineSeg(contour.points[pointIndex], firstPt, distance, pointIndex);
    if (distance > prevD) {
      contour.points.push_back(firstPt);
    }
  }

  contour.length = distance;
  contour.isClosed = haveSeenClose;

  if (contour.points.empty()) {
    return std::nullopt;
  }

  return contour;
}

// ---------------------------------------------------------------------------
// dashImpl
// ---------------------------------------------------------------------------

std::optional<Path> dashImpl(const Path& src, const StrokeDash& dash, float resScale) {
  // We do not support the `cull_path` branch here.
  // Skia has a lot of code for cases when a path contains only a single
  // zero-length line or when a path is a rect. Not sure why.
  // We simply ignore it for the sake of simplicity.

  // We also don't support the `SpecialLineRec` case.

  auto params = DashParams::create(dash);
  if (!params.has_value()) {
    return std::nullopt;
  }

  PathBuilder pb;
  float dashCount = 0.0f;

  ContourMeasureIter contourIter(src, resScale);
  while (auto contour = contourIter.next()) {
    bool skipFirstSegment = contour->isClosed;
    bool addedSegment = false;
    float length = contour->length;
    std::size_t index = params->firstIndex;

    // Since the path length / dash length ratio may be arbitrarily large,
    // we can exert significant memory pressure while attempting to build
    // the filtered path. To avoid this, we simply give up dashing beyond a
    // certain threshold.
    //
    // The original bug report (http://crbug.com/165432) is based on a path
    // yielding more than 90 million dash segments and crashing the memory
    // allocator. A limit of 1 million segments seems reasonable: at 2 verbs
    // per segment * 9 bytes per verb, this caps the maximum dash memory
    // overhead at roughly 17 MB per path.
    constexpr float kMaxDashCount = 1000000.0f;
    dashCount += length * static_cast<float>(dash.array.size() >> 1) / params->intervalLen;
    if (dashCount > kMaxDashCount) {
      return std::nullopt;
    }

    // Using double precision to avoid looping indefinitely due to single
    // precision rounding (for extreme path_length/dash_length ratios).
    float distance = 0.0f;
    float dLen = params->firstLen;

    while (distance < length) {
      assert(dLen >= 0.0f);
      addedSegment = false;
      if (isEven(index) && !skipFirstSegment) {
        addedSegment = true;
        contour->pushSegment(distance, distance + dLen, true, pb);
      }

      distance += dLen;

      // clear this so we only respect it the first time around
      skipFirstSegment = false;

      // wrap around our intervals array if necessary
      index += 1;
      assert(index <= dash.array.size());
      if (index == dash.array.size()) {
        index = 0;
      }

      // fetch our next dLen
      dLen = dash.array[index];
    }

    // extend if we ended on a segment and we need to join up with
    // the (skipped) initial segment
    if (contour->isClosed && isEven(params->firstIndex) && params->firstLen >= 0.0f) {
      contour->pushSegment(0.0f, params->firstLen, !addedSegment, pb);
    }
  }

  return pb.finish();
}

// ---------------------------------------------------------------------------
// Path::dash
// ---------------------------------------------------------------------------

std::optional<Path> Path::dash(const StrokeDash& dashPattern, float resScale) const {
  return dashImpl(*this, dashPattern, resScale);
}

}  // namespace tiny_skia
