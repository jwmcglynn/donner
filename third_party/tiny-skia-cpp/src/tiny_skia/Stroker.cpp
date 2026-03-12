// Copyright 2008 The Android Open Source Project
// Copyright 2020 Yevhenii Reizner
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Based on SkStroke.cpp
// C++ port of tiny-skia/path/src/stroker.rs

#include "tiny_skia/Stroker.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "tiny_skia/Math.h"
#include "tiny_skia/PathGeometry.h"

namespace tiny_skia {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int kQuadRecursiveLimit = 3;

// quads with extreme widths recurse to point of failure
// largest seen for normal cubics: 5, 26
// largest seen for normal quads: 11
static constexpr int RECURSIVE_LIMITS[4] = {
    5 * 3,  // 3x limits seen in practice
    26 * 3,
    11 * 3,
    11 * 3,
};

// ---------------------------------------------------------------------------
// QuadConstruct
// ---------------------------------------------------------------------------

bool QuadConstruct::init(NormalizedF32 start, NormalizedF32 end) {
  startT = start;
  midT = NormalizedF32::newClamped((start.get() + end.get()) * 0.5f);
  endT = end;
  startSet = false;
  endSet = false;
  return startT < midT && midT < endT;
}

bool QuadConstruct::initWithStart(const QuadConstruct& parent) {
  if (!init(parent.startT, parent.midT)) {
    return false;
  }
  quad[0] = parent.quad[0];
  tangentStart = parent.tangentStart;
  startSet = true;
  return true;
}

bool QuadConstruct::initWithEnd(const QuadConstruct& parent) {
  if (!init(parent.midT, parent.endT)) {
    return false;
  }
  quad[2] = parent.quad[2];
  tangentEnd = parent.tangentEnd;
  endSet = true;
  return true;
}

// ---------------------------------------------------------------------------
// Free helper functions
// ---------------------------------------------------------------------------

bool setNormalUnitNormal(Point before, Point after, float scale, float radius, Point& normal,
                         Point& unitNormal) {
  if (!unitNormal.setNormalize((after.x - before.x) * scale, (after.y - before.y) * scale)) {
    return false;
  }
  unitNormal.rotateCounterClockwise();
  normal = unitNormal.scaled(radius);
  return true;
}

bool setNormalUnitNormal2(Point vec, float radius, Point& normal, Point& unitNormal) {
  if (!unitNormal.setNormalize(vec.x, vec.y)) {
    return false;
  }
  unitNormal.rotateCounterClockwise();
  normal = unitNormal.scaled(radius);
  return true;
}

bool isClockwise(Point before, Point after) { return before.x * after.y > before.y * after.x; }

AngleType dotToAngleType(float dot) {
  if (dot >= 0.0f) {
    // shallow or line
    if (isNearlyZero(1.0f - dot)) {
      return AngleType::NearlyLine;
    } else {
      return AngleType::Shallow;
    }
  } else {
    // sharp or 180
    if (isNearlyZero(1.0f + dot)) {
      return AngleType::Nearly180;
    } else {
      return AngleType::Sharp;
    }
  }
}

void handleInnerJoin(Point pivot, Point after, PathBuilder& inner) {
  inner.lineTo(pivot.x, pivot.y);
  inner.lineTo(pivot.x - after.x, pivot.y - after.y);
}

bool degenerateVector(Point v) { return !v.canNormalize(); }

// returns the distance squared from the point to the line
float ptToLine(Point pt, Point lineStart, Point lineEnd) {
  Point dxy = lineEnd - lineStart;
  Point ab0 = pt - lineStart;
  float numer = dxy.dot(ab0);
  float denom = dxy.dot(dxy);
  float t = numer / denom;
  if (t >= 0.0f && t <= 1.0f) {
    Point hit = Point::fromXY(lineStart.x * (1.0f - t) + lineEnd.x * t,
                              lineStart.y * (1.0f - t) + lineEnd.y * t);
    return hit.distanceToSquared(pt);
  } else {
    return pt.distanceToSquared(lineStart);
  }
}

// Given quad, see if all three points are in a line.
bool quadInLine(const Point quad[3]) {
  float ptMax = -1.0f;
  int outer1 = 0;
  int outer2 = 0;
  for (int index = 0; index < 2; ++index) {
    for (int inner = index + 1; inner < 3; ++inner) {
      Point testDiff = quad[inner] - quad[index];
      float testMax = std::max(std::abs(testDiff.x), std::abs(testDiff.y));
      if (ptMax < testMax) {
        outer1 = index;
        outer2 = inner;
        ptMax = testMax;
      }
    }
  }

  int mid = outer1 ^ outer2 ^ 3;
  constexpr float CURVATURE_SLOP = 0.000005f;
  float lineSlop = ptMax * ptMax * CURVATURE_SLOP;
  return ptToLine(quad[mid], quad[outer1], quad[outer2]) <= lineSlop;
}

// Given a cubic, determine if all four points are in a line.
bool cubicInLine(const Point cubic[4]) {
  float ptMax = -1.0f;
  int outer1 = 0;
  int outer2 = 0;
  for (int index = 0; index < 3; ++index) {
    for (int inner = index + 1; inner < 4; ++inner) {
      Point testDiff = cubic[inner] - cubic[index];
      float testMax = std::max(std::abs(testDiff.x), std::abs(testDiff.y));
      if (ptMax < testMax) {
        outer1 = index;
        outer2 = inner;
        ptMax = testMax;
      }
    }
  }

  int mid1 = (1 + (2 >> outer2)) >> outer1;
  int mid2 = outer1 ^ outer2 ^ mid1;
  float lineSlop = ptMax * ptMax * 0.00001f;

  return ptToLine(cubic[mid1], cubic[outer1], cubic[outer2]) <= lineSlop &&
         ptToLine(cubic[mid2], cubic[outer1], cubic[outer2]) <= lineSlop;
}

std::pair<Point, ReductionType> checkQuadLinear(const Point quad[3]) {
  bool degenerateAb = degenerateVector(quad[1] - quad[0]);
  bool degenerateBc = degenerateVector(quad[2] - quad[1]);
  if (degenerateAb && degenerateBc) {
    return {Point::zero(), ReductionType::Point};
  }

  if (degenerateAb || degenerateBc) {
    return {Point::zero(), ReductionType::Line};
  }

  if (!quadInLine(quad)) {
    return {Point::zero(), ReductionType::Quad};
  }

  NormalizedF32 t = pathGeometry::findQuadMaxCurvature(quad);
  if (t == NormalizedF32::ZERO || t == NormalizedF32::ONE) {
    return {Point::zero(), ReductionType::Line};
  }

  return {pathGeometry::evalQuadAt(quad, t), ReductionType::Degenerate};
}

ReductionType checkCubicLinear(const Point cubic[4], Point reduction[3], Point* tangentPt) {
  bool degenerateAb = degenerateVector(cubic[1] - cubic[0]);
  bool degenerateBc = degenerateVector(cubic[2] - cubic[1]);
  bool degenerateCd = degenerateVector(cubic[3] - cubic[2]);
  if (degenerateAb && degenerateBc && degenerateCd) {
    return ReductionType::Point;
  }

  if ((int)degenerateAb + (int)degenerateBc + (int)degenerateCd == 2) {
    return ReductionType::Line;
  }

  if (!cubicInLine(cubic)) {
    if (tangentPt) {
      *tangentPt = degenerateAb ? cubic[2] : cubic[1];
    }
    return ReductionType::Quad;
  }

  NormalizedF32 tValues[3] = {NormalizedF32::ZERO, NormalizedF32::ZERO, NormalizedF32::ZERO};
  auto tSlice = pathGeometry::findCubicMaxCurvatureTs(cubic, tValues);
  int rCount = 0;
  for (std::size_t i = 0; i < tSlice; ++i) {
    float tv = tValues[i].get();
    if (0.0f >= tv || tv >= 1.0f) {
      continue;
    }
    reduction[rCount] = pathGeometry::evalCubicPosAt(cubic, tValues[i]);
    if (!(reduction[rCount] == cubic[0]) && !(reduction[rCount] == cubic[3])) {
      rCount += 1;
    }
  }

  switch (rCount) {
    case 0:
      return ReductionType::Line;
    case 1:
      return ReductionType::Degenerate;
    case 2:
      return ReductionType::Degenerate2;
    case 3:
      return ReductionType::Degenerate3;
    default:
      return ReductionType::Line;
  }
}

std::size_t intersectQuadRay(const Point line[2], const Point quad[3],
                             NormalizedF32Exclusive roots[3]) {
  Point vec = line[1] - line[0];
  float r[3];
  for (int n = 0; n < 3; ++n) {
    r[n] = (quad[n].y - line[0].y) * vec.x - (quad[n].x - line[0].x) * vec.y;
  }
  float a = r[2];
  float b = r[1];
  float c = r[0];
  a += c - 2.0f * b;  // A = a - 2*b + c
  b -= c;             // B = -(b - c)

  return pathGeometry::findUnitQuadRoots(a, 2.0f * b, c, roots);
}

bool pointsWithinDist(Point nearPt, Point farPt, float limit) {
  return nearPt.distanceToSquared(farPt) <= limit * limit;
}

bool sharpAngle(const Point quad[3]) {
  Point smaller = quad[1] - quad[0];
  Point larger = quad[1] - quad[2];
  float smallerLen = smaller.lengthSquared();
  float largerLen = larger.lengthSquared();
  if (smallerLen > largerLen) {
    std::swap(smaller, larger);
    largerLen = smallerLen;
  }

  if (!smaller.setLength(largerLen)) {
    return false;
  }

  float dot = smaller.dot(larger);
  return dot > 0.0f;
}

bool ptInQuadBounds(const Point quad[3], Point pt, float invResScale) {
  float xMin = std::min({quad[0].x, quad[1].x, quad[2].x});
  if (pt.x + invResScale < xMin) {
    return false;
  }

  float xMax = std::max({quad[0].x, quad[1].x, quad[2].x});
  if (pt.x - invResScale > xMax) {
    return false;
  }

  float yMin = std::min({quad[0].y, quad[1].y, quad[2].y});
  if (pt.y + invResScale < yMin) {
    return false;
  }

  float yMax = std::max({quad[0].y, quad[1].y, quad[2].y});
  if (pt.y - invResScale > yMax) {
    return false;
  }

  return true;
}

// ---------------------------------------------------------------------------
// Cap functions
// ---------------------------------------------------------------------------

void buttCapper(Point /*pivot*/, Point /*normal*/, Point stop, const PathBuilder* /*otherPath*/,
                PathBuilder& path) {
  path.lineTo(stop.x, stop.y);
}

void roundCapper(Point pivot, Point normal, Point stop, const PathBuilder* /*otherPath*/,
                 PathBuilder& path) {
  Point parallel = normal;
  parallel.rotateClockwise();

  Point projectedCenter = pivot + parallel;

  path.conicPointsTo(projectedCenter + normal, projectedCenter, kScalarRoot2Over2);
  path.conicPointsTo(projectedCenter - normal, stop, kScalarRoot2Over2);
}

void squareCapper(Point pivot, Point normal, Point stop, const PathBuilder* otherPath,
                  PathBuilder& path) {
  Point parallel = normal;
  parallel.rotateClockwise();

  if (otherPath != nullptr) {
    path.setLastPoint(
        Point::fromXY(pivot.x + normal.x + parallel.x, pivot.y + normal.y + parallel.y));
    path.lineTo(pivot.x - normal.x + parallel.x, pivot.y - normal.y + parallel.y);
  } else {
    path.lineTo(pivot.x + normal.x + parallel.x, pivot.y + normal.y + parallel.y);
    path.lineTo(pivot.x - normal.x + parallel.x, pivot.y - normal.y + parallel.y);
    path.lineTo(stop.x, stop.y);
  }
}

CapProc capFactory(LineCap cap) {
  switch (cap) {
    case LineCap::Butt:
      return buttCapper;
    case LineCap::Round:
      return roundCapper;
    case LineCap::Square:
      return squareCapper;
  }
  return buttCapper;
}

// ---------------------------------------------------------------------------
// Join functions
// ---------------------------------------------------------------------------

void bevelJoiner(Point beforeUnitNormal, Point pivot, Point afterUnitNormal, float radius,
                 float /*invMiterLimit*/, bool /*prevIsLine*/, bool /*currIsLine*/,
                 SwappableBuilders builders) {
  Point after = afterUnitNormal.scaled(radius);

  if (!isClockwise(beforeUnitNormal, afterUnitNormal)) {
    builders.swap();
    after = -after;
  }

  builders.outer->lineTo(pivot.x + after.x, pivot.y + after.y);
  handleInnerJoin(pivot, after, *builders.inner);
}

void roundJoiner(Point beforeUnitNormal, Point pivot, Point afterUnitNormal, float radius,
                 float /*invMiterLimit*/, bool /*prevIsLine*/, bool /*currIsLine*/,
                 SwappableBuilders builders) {
  float dotProd = beforeUnitNormal.dot(afterUnitNormal);
  AngleType angleType = dotToAngleType(dotProd);

  if (angleType == AngleType::NearlyLine) {
    return;
  }

  Point before = beforeUnitNormal;
  Point after = afterUnitNormal;
  PathDirection dir = PathDirection::CW;

  if (!isClockwise(before, after)) {
    builders.swap();
    before = -before;
    after = -after;
    dir = PathDirection::CCW;
  }

  Transform ts = Transform::fromRow(radius, 0.0f, 0.0f, radius, pivot.x, pivot.y);

  pathGeometry::Conic conics[5];
  auto conicsSpan = pathGeometry::Conic::buildUnitArc(before, after, dir, ts, conics);
  if (conicsSpan.has_value()) {
    for (const auto& conic : conicsSpan.value()) {
      builders.outer->conicPointsTo(conic.points[1], conic.points[2], conic.weight);
    }
    after.scale(radius);
    handleInnerJoin(pivot, after, *builders.inner);
  }
}

// Inner helper for miter joiners.
static void doBluntOrClipped(SwappableBuilders builders, Point pivot, float radius, bool prevIsLine,
                             bool currIsLine, Point before, Point mid, Point after,
                             float invMiterLimit, bool miterClip) {
  after.scale(radius);

  if (miterClip) {
    mid.normalize();

    float cosBeta = before.dot(mid);
    float sinBeta = before.cross(mid);

    float x;
    if (std::abs(sinBeta) <= kScalarNearlyZero) {
      x = 1.0f / invMiterLimit;
    } else {
      x = ((1.0f / invMiterLimit) - cosBeta) / sinBeta;
    }

    before.scale(radius);

    Point beforeTangent = before;
    beforeTangent.rotateClockwise();

    Point afterTangent = after;
    afterTangent.rotateCounterClockwise();

    Point c1 = pivot + before + beforeTangent.scaled(x);
    Point c2 = pivot + after + afterTangent.scaled(x);

    if (prevIsLine) {
      builders.outer->setLastPoint(c1);
    } else {
      builders.outer->lineTo(c1.x, c1.y);
    }

    builders.outer->lineTo(c2.x, c2.y);
  }

  if (!currIsLine) {
    builders.outer->lineTo(pivot.x + after.x, pivot.y + after.y);
  }

  handleInnerJoin(pivot, after, *builders.inner);
}

static void doMiter(SwappableBuilders builders, Point pivot, float radius, bool prevIsLine,
                    bool currIsLine, Point mid, Point after) {
  after.scale(radius);

  if (prevIsLine) {
    builders.outer->setLastPoint(Point::fromXY(pivot.x + mid.x, pivot.y + mid.y));
  } else {
    builders.outer->lineTo(pivot.x + mid.x, pivot.y + mid.y);
  }

  if (!currIsLine) {
    builders.outer->lineTo(pivot.x + after.x, pivot.y + after.y);
  }

  handleInnerJoin(pivot, after, *builders.inner);
}

static void miterJoinerInner(Point beforeUnitNormal, Point pivot, Point afterUnitNormal,
                             float radius, float invMiterLimit, bool miterClip, bool prevIsLine,
                             bool currIsLine, SwappableBuilders builders) {
  // negate the dot since we're using normals instead of tangents
  float dotProd = beforeUnitNormal.dot(afterUnitNormal);
  AngleType angleType = dotToAngleType(dotProd);
  Point before = beforeUnitNormal;
  Point after = afterUnitNormal;
  Point mid;

  if (angleType == AngleType::NearlyLine) {
    return;
  }

  if (angleType == AngleType::Nearly180) {
    currIsLine = false;
    mid = (after - before).scaled(radius / 2.0f);
    doBluntOrClipped(builders, pivot, radius, prevIsLine, currIsLine, before, mid, after,
                     invMiterLimit, miterClip);
    return;
  }

  bool ccw = !isClockwise(before, after);
  if (ccw) {
    builders.swap();
    before = -before;
    after = -after;
  }

  // Before we enter the world of square-roots and divides,
  // check if we're trying to join an upright right angle
  if (dotProd == 0.0f && invMiterLimit <= kScalarRoot2Over2) {
    mid = (before + after).scaled(radius);
    doMiter(builders, pivot, radius, prevIsLine, currIsLine, mid, after);
    return;
  }

  // choose the most accurate way to form the initial mid-vector
  if (angleType == AngleType::Sharp) {
    mid = Point::fromXY(after.y - before.y, before.x - after.x);
    if (ccw) {
      mid = -mid;
    }
  } else {
    mid = Point::fromXY(before.x + after.x, before.y + after.y);
  }

  // midLength = radius / sinHalfAngle
  float sinHalfAngle = std::sqrt((1.0f + dotProd) * 0.5f);
  if (sinHalfAngle < invMiterLimit) {
    currIsLine = false;
    doBluntOrClipped(builders, pivot, radius, prevIsLine, currIsLine, before, mid, after,
                     invMiterLimit, miterClip);
    return;
  }

  mid.setLength(radius / sinHalfAngle);
  doMiter(builders, pivot, radius, prevIsLine, currIsLine, mid, after);
}

void miterJoiner(Point beforeUnitNormal, Point pivot, Point afterUnitNormal, float radius,
                 float invMiterLimit, bool prevIsLine, bool currIsLine,
                 SwappableBuilders builders) {
  miterJoinerInner(beforeUnitNormal, pivot, afterUnitNormal, radius, invMiterLimit,
                   /*miterClip=*/false, prevIsLine, currIsLine, builders);
}

void miterClipJoiner(Point beforeUnitNormal, Point pivot, Point afterUnitNormal, float radius,
                     float invMiterLimit, bool prevIsLine, bool currIsLine,
                     SwappableBuilders builders) {
  miterJoinerInner(beforeUnitNormal, pivot, afterUnitNormal, radius, invMiterLimit,
                   /*miterClip=*/true, prevIsLine, currIsLine, builders);
}

JoinProc joinFactory(LineJoin join) {
  switch (join) {
    case LineJoin::Miter:
      return miterJoiner;
    case LineJoin::MiterClip:
      return miterClipJoiner;
    case LineJoin::Round:
      return roundJoiner;
    case LineJoin::Bevel:
      return bevelJoiner;
  }
  return miterJoiner;
}

// ---------------------------------------------------------------------------
// PathStroker
// ---------------------------------------------------------------------------

PathStroker::PathStroker()
    : radius_(0.0f),
      invMiterLimit_(0.0f),
      resScale_(1.0f),
      invResScale_(1.0f),
      invResScaleSquared_(1.0f),
      firstNormal_(Point::zero()),
      prevNormal_(Point::zero()),
      firstUnitNormal_(Point::zero()),
      prevUnitNormal_(Point::zero()),
      firstPt_(Point::zero()),
      prevPt_(Point::zero()),
      firstOuterPt_(Point::zero()),
      firstOuterPtIndexInContour_(0),
      segmentCount_(-1),
      prevIsLine_(false),
      capper_(buttCapper),
      joiner_(miterJoiner),
      inner_(),
      outer_(),
      cusper_(),
      strokeType_(StrokeType::Outer),
      recursionDepth_(0),
      foundTangents_(false),
      joinCompleted_(false) {}

float PathStroker::computeResolutionScale(const Transform& ts) {
  float sx = Point::fromXY(ts.sx, ts.kx).length();
  float sy = Point::fromXY(ts.ky, ts.sy).length();
  if (std::isfinite(sx) && std::isfinite(sy)) {
    float scale = std::max(sx, sy);
    if (scale > 0.0f) {
      return scale;
    }
  }
  return 1.0f;
}

std::optional<Path> PathStroker::stroke(const Path& path, const Stroke& stroke,
                                        float resolutionScale) {
  if (stroke.width <= 0.0f || !std::isfinite(stroke.width)) {
    return std::nullopt;
  }
  return strokeInner(path, stroke.width, stroke.miterLimit, stroke.lineCap, stroke.lineJoin,
                     resolutionScale);
}

std::optional<Path> PathStroker::strokeInner(const Path& path, float width, float miterLimit,
                                             LineCap lineCap, LineJoin lineJoin, float resScale) {
  float invMiterLimit = 0.0f;

  if (lineJoin == LineJoin::Miter) {
    if (miterLimit <= 1.0f) {
      lineJoin = LineJoin::Bevel;
    } else {
      invMiterLimit = 1.0f / miterLimit;
    }
  }

  if (lineJoin == LineJoin::MiterClip) {
    invMiterLimit = 1.0f / miterLimit;
  }

  resScale_ = resScale;
  radius_ = width * 0.5f;
  // The '4' below matches the fill scan converter's error term.
  float baseInvResScale = 1.0f / (resScale * 4.0f);
  // For thin strokes (radius < 2px), loosen tolerance proportionally.
  // Subpixel precision is wasted when the stroke is only a few pixels wide.
  if (radius_ > 0.0f && radius_ < 2.0f) {
    float scale = std::min(2.0f / radius_, 2.0f);
    invResScale_ = baseInvResScale * scale;
  } else {
    invResScale_ = baseInvResScale;
  }
  invResScaleSquared_ = invResScale_ * invResScale_;
  invMiterLimit_ = invMiterLimit;

  firstNormal_ = Point::zero();
  prevNormal_ = Point::zero();
  firstUnitNormal_ = Point::zero();
  prevUnitNormal_ = Point::zero();

  firstPt_ = Point::zero();
  prevPt_ = Point::zero();

  firstOuterPt_ = Point::zero();
  firstOuterPtIndexInContour_ = 0;
  segmentCount_ = -1;
  prevIsLine_ = false;

  capper_ = capFactory(lineCap);
  joiner_ = joinFactory(lineJoin);

  // For thin strokes, use looser conic tolerance to generate fewer quads from
  // round caps/joins. The default 0.25 is tight for small radius arcs.
  if (radius_ > 0.0f && radius_ < 4.0f) {
    float conicTol = std::min(1.0f, 0.25f * (4.0f / radius_));
    inner_.setConicTolerance(conicTol);
    outer_.setConicTolerance(conicTol);
    cusper_.setConicTolerance(conicTol);
  }

  // Reserve space based on input path size.
  inner_.clear();
  inner_.reserve(path.verbs().size(), path.points().size());

  outer_.clear();
  outer_.reserve(path.verbs().size() * 3, path.points().size() * 3);

  cusper_.clear();

  strokeType_ = StrokeType::Outer;

  recursionDepth_ = 0;
  foundTangents_ = false;
  joinCompleted_ = false;

  bool lastSegmentIsLine = false;
  PathSegmentsIter iter(path);
  iter.setAutoClose(true);
  while (auto segment = iter.next()) {
    switch (segment->kind) {
      case PathSegment::Kind::MoveTo:
        moveTo(segment->pts[0]);
        break;
      case PathSegment::Kind::LineTo:
        lineTo(segment->pts[0], &iter);
        lastSegmentIsLine = true;
        break;
      case PathSegment::Kind::QuadTo:
        quadTo(segment->pts[0], segment->pts[1]);
        lastSegmentIsLine = false;
        break;
      case PathSegment::Kind::CubicTo:
        cubicTo(segment->pts[0], segment->pts[1], segment->pts[2]);
        lastSegmentIsLine = false;
        break;
      case PathSegment::Kind::Close:
        if (lineCap != LineCap::Butt) {
          if (hasOnlyMoveTo()) {
            lineTo(moveToPt(), nullptr);
            lastSegmentIsLine = true;
            continue;
          }
          if (isCurrentContourEmpty()) {
            lastSegmentIsLine = true;
            continue;
          }
        }
        closeContour(lastSegmentIsLine);
        break;
    }
  }

  return finish(lastSegmentIsLine);
}

SwappableBuilders PathStroker::builders() { return SwappableBuilders{&inner_, &outer_}; }

Point PathStroker::moveToPt() const { return firstPt_; }

void PathStroker::moveTo(Point p) {
  if (segmentCount_ > 0) {
    finishContour(false, false);
  }

  segmentCount_ = 0;
  firstPt_ = p;
  prevPt_ = p;
  joinCompleted_ = false;
}

void PathStroker::lineTo(Point p, const PathSegmentsIter* iter) {
  bool teenyLine = prevPt_.equalsWithinTolerance(p, kScalarNearlyZero * invResScale_);
  if (capper_ == buttCapper && teenyLine) {
    return;
  }

  if (teenyLine && (joinCompleted_ || (iter != nullptr && iter->hasValidTangent()))) {
    return;
  }

  Point normal = Point::zero();
  Point unitNormal = Point::zero();
  if (!preJoinTo(p, true, normal, unitNormal)) {
    return;
  }

  outer_.lineTo(p.x + normal.x, p.y + normal.y);
  inner_.lineTo(p.x - normal.x, p.y - normal.y);

  postJoinTo(p, normal, unitNormal);
}

void PathStroker::quadTo(Point p1, Point p2) {
  Point quad[3] = {prevPt_, p1, p2};
  auto [reduction, reductionType] = checkQuadLinear(quad);
  if (reductionType == ReductionType::Point) {
    lineTo(p2, nullptr);
    return;
  }

  if (reductionType == ReductionType::Line) {
    lineTo(p2, nullptr);
    return;
  }

  if (reductionType == ReductionType::Degenerate) {
    lineTo(reduction, nullptr);
    JoinProc saveJoiner = joiner_;
    joiner_ = roundJoiner;
    lineTo(p2, nullptr);
    joiner_ = saveJoiner;
    return;
  }

  // reductionType == Quad
  Point normalAb = Point::zero();
  Point unitAb = Point::zero();
  Point normalBc = Point::zero();
  Point unitBc = Point::zero();
  if (!preJoinTo(p1, false, normalAb, unitAb)) {
    lineTo(p2, nullptr);
    return;
  }

  QuadConstruct quadPoints;
  initQuad(StrokeType::Outer, NormalizedF32::ZERO, NormalizedF32::ONE, quadPoints);
  quadStroke(quad, quadPoints);
  initQuad(StrokeType::Inner, NormalizedF32::ZERO, NormalizedF32::ONE, quadPoints);
  quadStroke(quad, quadPoints);

  bool ok = setNormalUnitNormal(quad[1], quad[2], resScale_, radius_, normalBc, unitBc);
  if (!ok) {
    normalBc = normalAb;
    unitBc = unitAb;
  }

  postJoinTo(p2, normalBc, unitBc);
}

void PathStroker::cubicTo(Point pt1, Point pt2, Point pt3) {
  Point cubic[4] = {prevPt_, pt1, pt2, pt3};
  Point reduction[3] = {Point::zero(), Point::zero(), Point::zero()};
  Point tangentPt = Point::zero();
  ReductionType reductionType = checkCubicLinear(cubic, reduction, &tangentPt);
  if (reductionType == ReductionType::Point) {
    lineTo(pt3, nullptr);
    return;
  }

  if (reductionType == ReductionType::Line) {
    lineTo(pt3, nullptr);
    return;
  }

  if (reductionType >= ReductionType::Degenerate && reductionType <= ReductionType::Degenerate3) {
    lineTo(reduction[0], nullptr);
    JoinProc saveJoiner = joiner_;
    joiner_ = roundJoiner;
    if (reductionType >= ReductionType::Degenerate2) {
      lineTo(reduction[1], nullptr);
    }
    if (reductionType == ReductionType::Degenerate3) {
      lineTo(reduction[2], nullptr);
    }
    lineTo(pt3, nullptr);
    joiner_ = saveJoiner;
    return;
  }

  // reductionType == Quad
  Point normalAb = Point::zero();
  Point unitAb = Point::zero();
  Point normalCd = Point::zero();
  Point unitCd = Point::zero();
  if (!preJoinTo(tangentPt, false, normalAb, unitAb)) {
    lineTo(pt3, nullptr);
    return;
  }

  NormalizedF32Exclusive tValuesStorage[3] = {
      NormalizedF32Exclusive::HALF, NormalizedF32Exclusive::HALF, NormalizedF32Exclusive::HALF};
  std::size_t tCount = pathGeometry::findCubicInflections(cubic, tValuesStorage);

  NormalizedF32 lastT = NormalizedF32::ZERO;
  for (std::size_t index = 0; index <= tCount; ++index) {
    NormalizedF32 nextT;
    if (index < tCount) {
      nextT = tValuesStorage[index].toNormalized();
    } else {
      nextT = NormalizedF32::ONE;
    }

    QuadConstruct quadPoints;
    initQuad(StrokeType::Outer, lastT, nextT, quadPoints);
    cubicStroke(cubic, quadPoints);
    initQuad(StrokeType::Inner, lastT, nextT, quadPoints);
    cubicStroke(cubic, quadPoints);
    lastT = nextT;
  }

  auto cusp = pathGeometry::findCubicCusp(cubic);
  if (cusp.has_value()) {
    Point cuspLoc = pathGeometry::evalCubicPosAt(cubic, cusp->toNormalized());
    cusper_.pushCircle(cuspLoc.x, cuspLoc.y, radius_);
  }

  // emit the join even if one stroke succeeded but the last one failed
  setCubicEndNormal(cubic, normalAb, unitAb, normalCd, unitCd);

  postJoinTo(pt3, normalCd, unitCd);
}

bool PathStroker::cubicStroke(const Point cubic[4], QuadConstruct& quadPoints) {
  if (!foundTangents_) {
    ResultType resultType = tangentsMeet(cubic, quadPoints);
    if (resultType != ResultType::Quad) {
      bool ok = pointsWithinDist(quadPoints.quad[0], quadPoints.quad[2], invResScale_);
      if ((resultType == ResultType::Degenerate || ok) && cubicMidOnLine(cubic, quadPoints)) {
        addDegenerateLine(quadPoints);
        return true;
      }
    } else {
      foundTangents_ = true;
    }
  }

  if (foundTangents_) {
    ResultType resultType = compareQuadCubic(cubic, quadPoints);
    if (resultType == ResultType::Quad) {
      const Point* stroke = quadPoints.quad;
      if (strokeType_ == StrokeType::Outer) {
        outer_.quadTo(stroke[1].x, stroke[1].y, stroke[2].x, stroke[2].y);
      } else {
        inner_.quadTo(stroke[1].x, stroke[1].y, stroke[2].x, stroke[2].y);
      }
      return true;
    }

    if (resultType == ResultType::Degenerate) {
      if (!quadPoints.oppositeTangents) {
        addDegenerateLine(quadPoints);
        return true;
      }
    }
  }

  if (!std::isfinite(quadPoints.quad[2].x) || !std::isfinite(quadPoints.quad[2].y)) {
    return false;  // just abort if projected quad isn't representable
  }

  recursionDepth_ += 1;
  if (recursionDepth_ > RECURSIVE_LIMITS[foundTangents_ ? 1 : 0]) {
    return false;  // just abort if projected quad isn't representable
  }

  QuadConstruct half;
  if (!half.initWithStart(quadPoints)) {
    addDegenerateLine(quadPoints);
    recursionDepth_ -= 1;
    return true;
  }

  if (!cubicStroke(cubic, half)) {
    return false;
  }

  if (!half.initWithEnd(quadPoints)) {
    addDegenerateLine(quadPoints);
    recursionDepth_ -= 1;
    return true;
  }

  if (!cubicStroke(cubic, half)) {
    return false;
  }

  recursionDepth_ -= 1;
  return true;
}

bool PathStroker::cubicMidOnLine(const Point cubic[4], QuadConstruct& quadPoints) {
  Point strokeMid = Point::zero();
  cubicQuadMid(cubic, quadPoints, strokeMid);
  float dist = ptToLine(strokeMid, quadPoints.quad[0], quadPoints.quad[2]);
  return dist < invResScaleSquared_;
}

void PathStroker::cubicQuadMid(const Point cubic[4], QuadConstruct& quadPoints, Point& mid) {
  Point cubicMidPt = Point::zero();
  cubicPerpRay(cubic, quadPoints.midT, cubicMidPt, mid, nullptr);
}

void PathStroker::cubicPerpRay(const Point cubic[4], NormalizedF32 t, Point& tPt, Point& onPt,
                               Point* tangent) {
  tPt = pathGeometry::evalCubicPosAt(cubic, t);
  Point dxy = pathGeometry::evalCubicTangentAt(cubic, t);

  Point chopped[7];
  const Point* cPoints = cubic;
  if (dxy.x == 0.0f && dxy.y == 0.0f) {
    if (isNearlyZero(t.get())) {
      dxy = cubic[2] - cubic[0];
    } else if (isNearlyZero(1.0f - t.get())) {
      dxy = cubic[3] - cubic[1];
    } else {
      // If the cubic inflection falls on the cusp, subdivide the cubic
      // to find the tangent at that point.
      auto tExcl = NormalizedF32Exclusive::create(t.get());
      if (tExcl.has_value()) {
        pathGeometry::chopCubicAt2(cubic, *tExcl, chopped);
        dxy = chopped[3] - chopped[2];
        if (dxy.x == 0.0f && dxy.y == 0.0f) {
          dxy = chopped[3] - chopped[1];
          cPoints = chopped;
        }
      }
    }

    if (dxy.x == 0.0f && dxy.y == 0.0f) {
      dxy = cPoints[3] - cPoints[0];
    }
  }

  setRayPoints(tPt, dxy, onPt, tangent);
}

void PathStroker::setCubicEndNormal(const Point cubic[4], Point normalAb, Point unitNormalAb,
                                    Point& normalCd, Point& unitNormalCd) {
  Point ab = cubic[1] - cubic[0];
  Point cd = cubic[3] - cubic[2];

  bool degenerateAb = degenerateVector(ab);
  bool degenerateCb = degenerateVector(cd);

  if (degenerateAb && degenerateCb) {
    normalCd = normalAb;
    unitNormalCd = unitNormalAb;
    return;
  }

  if (degenerateAb) {
    ab = cubic[2] - cubic[0];
    degenerateAb = degenerateVector(ab);
  }

  if (degenerateCb) {
    cd = cubic[3] - cubic[1];
    degenerateCb = degenerateVector(cd);
  }

  if (degenerateAb || degenerateCb) {
    normalCd = normalAb;
    unitNormalCd = unitNormalAb;
    return;
  }

  setNormalUnitNormal2(cd, radius_, normalCd, unitNormalCd);
}

ResultType PathStroker::compareQuadCubic(const Point cubic[4], QuadConstruct& quadPoints) {
  // get the quadratic approximation of the stroke
  cubicQuadEnds(cubic, quadPoints);
  ResultType resultType = intersectRay(IntersectRayType::CtrlPt, quadPoints);
  if (resultType != ResultType::Quad) {
    return resultType;
  }

  // project a ray from the curve to the stroke
  Point ray0 = Point::zero();
  Point ray1 = Point::zero();
  cubicPerpRay(cubic, quadPoints.midT, ray1, ray0, nullptr);

  Point strokeCopy[3] = {quadPoints.quad[0], quadPoints.quad[1], quadPoints.quad[2]};
  Point ray[2] = {ray0, ray1};
  return strokeCloseEnough(strokeCopy, ray, quadPoints);
}

void PathStroker::cubicQuadEnds(const Point cubic[4], QuadConstruct& quadPoints) {
  if (!quadPoints.startSet) {
    Point cubicStartPt = Point::zero();
    cubicPerpRay(cubic, quadPoints.startT, cubicStartPt, quadPoints.quad[0],
                 &quadPoints.tangentStart);
    quadPoints.startSet = true;
  }

  if (!quadPoints.endSet) {
    Point cubicEndPt = Point::zero();
    cubicPerpRay(cubic, quadPoints.endT, cubicEndPt, quadPoints.quad[2], &quadPoints.tangentEnd);
    quadPoints.endSet = true;
  }
}

void PathStroker::closeContour(bool isLine) { finishContour(true, isLine); }

void PathStroker::finishContour(bool close, bool currIsLine) {
  if (segmentCount_ > 0) {
    if (close) {
      (joiner_)(prevUnitNormal_, prevPt_, firstUnitNormal_, radius_, invMiterLimit_, prevIsLine_,
                currIsLine, builders());
      outer_.close();

      // now add inner as its own contour
      auto pt = inner_.lastPoint().value_or(Point::zero());
      outer_.moveTo(pt.x, pt.y);
      outer_.reversePathTo(inner_);
      outer_.close();
    } else {
      // add caps to start and end

      // cap the end
      auto pt = inner_.lastPoint().value_or(Point::zero());
      const PathBuilder* otherPath = currIsLine ? &inner_ : nullptr;
      (capper_)(prevPt_, prevNormal_, pt, otherPath, outer_);
      outer_.reversePathTo(inner_);

      // cap the start
      const PathBuilder* otherPath2 = prevIsLine_ ? &inner_ : nullptr;
      (capper_)(firstPt_, -firstNormal_, firstOuterPt_, otherPath2, outer_);
      outer_.close();
    }

    if (!cusper_.empty()) {
      outer_.pushPathBuilder(cusper_);
      cusper_.clear();
    }
  }

  // since we may re-use `inner`, we clear instead of reset, to save on
  // reallocating its internal storage.
  inner_.clear();
  segmentCount_ = -1;
  firstOuterPtIndexInContour_ = outer_.points().size();
}

bool PathStroker::preJoinTo(Point p, bool currIsLine, Point& normal, Point& unitNormal) {
  float prevX = prevPt_.x;
  float prevY = prevPt_.y;

  bool normalSet = setNormalUnitNormal(prevPt_, p, resScale_, radius_, normal, unitNormal);
  if (!normalSet) {
    if (capper_ == buttCapper) {
      return false;
    }

    // Square caps and round caps draw even if the segment length is zero.
    normal = Point::fromXY(radius_, 0.0f);
    unitNormal = Point::fromXY(1.0f, 0.0f);
  }

  if (segmentCount_ == 0) {
    firstNormal_ = normal;
    firstUnitNormal_ = unitNormal;
    firstOuterPt_ = Point::fromXY(prevX + normal.x, prevY + normal.y);

    outer_.moveTo(firstOuterPt_.x, firstOuterPt_.y);
    inner_.moveTo(prevX - normal.x, prevY - normal.y);
  } else {
    // we have a previous segment
    (joiner_)(prevUnitNormal_, prevPt_, unitNormal, radius_, invMiterLimit_, prevIsLine_,
              currIsLine, builders());
  }
  prevIsLine_ = currIsLine;
  return true;
}

void PathStroker::postJoinTo(Point p, Point normal, Point unitNormal) {
  joinCompleted_ = true;
  prevPt_ = p;
  prevUnitNormal_ = unitNormal;
  prevNormal_ = normal;
  segmentCount_ += 1;
}

void PathStroker::initQuad(StrokeType strokeType, NormalizedF32 start, NormalizedF32 end,
                           QuadConstruct& quadPoints) {
  strokeType_ = strokeType;
  foundTangents_ = false;
  quadPoints.init(start, end);
}

bool PathStroker::quadStroke(const Point quad[3], QuadConstruct& quadPoints) {
  ResultType resultType = compareQuadQuad(quad, quadPoints);
  if (resultType == ResultType::Quad) {
    PathBuilder* path;
    if (strokeType_ == StrokeType::Outer) {
      path = &outer_;
    } else {
      path = &inner_;
    }

    path->quadTo(quadPoints.quad[1].x, quadPoints.quad[1].y, quadPoints.quad[2].x,
                 quadPoints.quad[2].y);
    return true;
  }

  if (resultType == ResultType::Degenerate) {
    addDegenerateLine(quadPoints);
    return true;
  }

  recursionDepth_ += 1;
  if (recursionDepth_ > RECURSIVE_LIMITS[kQuadRecursiveLimit]) {
    return false;
  }

  QuadConstruct half;
  half.initWithStart(quadPoints);
  if (!quadStroke(quad, half)) {
    return false;
  }

  half.initWithEnd(quadPoints);
  if (!quadStroke(quad, half)) {
    return false;
  }

  recursionDepth_ -= 1;
  return true;
}

ResultType PathStroker::compareQuadQuad(const Point quad[3], QuadConstruct& quadPoints) {
  // get the quadratic approximation of the stroke
  if (!quadPoints.startSet) {
    Point quadStartPt = Point::zero();
    quadPerpRay(quad, quadPoints.startT, quadStartPt, quadPoints.quad[0], &quadPoints.tangentStart);
    quadPoints.startSet = true;
  }

  if (!quadPoints.endSet) {
    Point quadEndPt = Point::zero();
    quadPerpRay(quad, quadPoints.endT, quadEndPt, quadPoints.quad[2], &quadPoints.tangentEnd);
    quadPoints.endSet = true;
  }

  ResultType resultType = intersectRay(IntersectRayType::CtrlPt, quadPoints);
  if (resultType != ResultType::Quad) {
    return resultType;
  }

  // project a ray from the curve to the stroke
  Point ray0 = Point::zero();
  Point ray1 = Point::zero();
  quadPerpRay(quad, quadPoints.midT, ray1, ray0, nullptr);

  Point strokeCopy[3] = {quadPoints.quad[0], quadPoints.quad[1], quadPoints.quad[2]};
  Point ray[2] = {ray0, ray1};
  return strokeCloseEnough(strokeCopy, ray, quadPoints);
}

void PathStroker::setRayPoints(Point tp, Point& dxy, Point& onP, Point* tangent) {
  if (!dxy.setLength(radius_)) {
    dxy = Point::fromXY(radius_, 0.0f);
  }

  float axisFlip = static_cast<float>(static_cast<int>(strokeType_));
  onP.x = tp.x + axisFlip * dxy.y;
  onP.y = tp.y - axisFlip * dxy.x;

  if (tangent != nullptr) {
    tangent->x = onP.x + dxy.x;
    tangent->y = onP.y + dxy.y;
  }
}

void PathStroker::quadPerpRay(const Point quad[3], NormalizedF32 t, Point& tp, Point& onP,
                              Point* tangent) {
  tp = pathGeometry::evalQuadAt(quad, t);
  Point dxy = pathGeometry::evalQuadTangentAt(quad, t);

  if (dxy.isZero()) {
    dxy = quad[2] - quad[0];
  }

  setRayPoints(tp, dxy, onP, tangent);
}

void PathStroker::addDegenerateLine(const QuadConstruct& quadPoints) {
  if (strokeType_ == StrokeType::Outer) {
    outer_.lineTo(quadPoints.quad[2].x, quadPoints.quad[2].y);
  } else {
    inner_.lineTo(quadPoints.quad[2].x, quadPoints.quad[2].y);
  }
}

ResultType PathStroker::strokeCloseEnough(const Point stroke[3], const Point ray[2],
                                          QuadConstruct& quadPoints) {
  NormalizedF32 half = NormalizedF32::newClamped(0.5f);
  Point strokeMid = pathGeometry::evalQuadAt(stroke, half);
  // measure the distance from the curve to the quad-stroke midpoint
  if (pointsWithinDist(ray[0], strokeMid, invResScale_)) {
    if (sharpAngle(quadPoints.quad)) {
      return ResultType::Split;
    }
    return ResultType::Quad;
  }

  // measure the distance to quad's bounds (quick reject)
  if (!ptInQuadBounds(stroke, ray[0], invResScale_)) {
    return ResultType::Split;
  }

  // measure the curve ray distance to the quad-stroke
  NormalizedF32Exclusive roots[3] = {NormalizedF32Exclusive::HALF, NormalizedF32Exclusive::HALF,
                                     NormalizedF32Exclusive::HALF};
  std::size_t rootCount = intersectQuadRay(ray, stroke, roots);
  if (rootCount != 1) {
    return ResultType::Split;
  }

  Point quadPt = pathGeometry::evalQuadAt(stroke, roots[0].toNormalized());
  float error = invResScale_ * (1.0f - std::abs(roots[0].get() - 0.5f) * 2.0f);
  if (pointsWithinDist(ray[0], quadPt, error)) {
    if (sharpAngle(quadPoints.quad)) {
      return ResultType::Split;
    }
    return ResultType::Quad;
  }

  // otherwise, subdivide
  return ResultType::Split;
}

ResultType PathStroker::intersectRay(IntersectRayType intersectRayType, QuadConstruct& quadPoints) {
  Point start = quadPoints.quad[0];
  Point end = quadPoints.quad[2];
  Point aLen = quadPoints.tangentStart - start;
  Point bLen = quadPoints.tangentEnd - end;

  float denom = aLen.cross(bLen);
  if (denom == 0.0f || !std::isfinite(denom)) {
    quadPoints.oppositeTangents = aLen.dot(bLen) < 0.0f;
    return ResultType::Degenerate;
  }

  quadPoints.oppositeTangents = false;
  Point ab0 = start - end;
  float numerA = bLen.cross(ab0);
  float numerB = aLen.cross(ab0);
  if ((numerA >= 0.0f) == (numerB >= 0.0f)) {
    // if the control point is outside the quad ends

    // if the perpendicular distances from the quad points to the opposite
    // tangent line are small, a straight line is good enough
    float dist1 = ptToLine(start, end, quadPoints.tangentEnd);
    float dist2 = ptToLine(end, start, quadPoints.tangentStart);
    if (std::max(dist1, dist2) <= invResScaleSquared_) {
      return ResultType::Degenerate;
    }

    return ResultType::Split;
  }

  // check to see if the denominator is teeny relative to the numerator
  numerA /= denom;
  bool validDivide = numerA > numerA - 1.0f;
  if (validDivide) {
    if (intersectRayType == IntersectRayType::CtrlPt) {
      quadPoints.quad[1].x = start.x * (1.0f - numerA) + quadPoints.tangentStart.x * numerA;
      quadPoints.quad[1].y = start.y * (1.0f - numerA) + quadPoints.tangentStart.y * numerA;
    }
    return ResultType::Quad;
  }

  quadPoints.oppositeTangents = aLen.dot(bLen) < 0.0f;

  // if the lines are parallel, straight line is good enough
  return ResultType::Degenerate;
}

ResultType PathStroker::tangentsMeet(const Point cubic[4], QuadConstruct& quadPoints) {
  cubicQuadEnds(cubic, quadPoints);
  return intersectRay(IntersectRayType::Result, quadPoints);
}

std::optional<Path> PathStroker::finish(bool isLine) {
  finishContour(false, isLine);

  // Swap out the outer builder.
  PathBuilder buf;
  std::swap(outer_, buf);

  return buf.finish();
}

bool PathStroker::hasOnlyMoveTo() const { return segmentCount_ == 0; }

bool PathStroker::isCurrentContourEmpty() const {
  return inner_.isZeroLengthSincePoint(0) &&
         outer_.isZeroLengthSincePoint(firstOuterPtIndexInContour_);
}

// ---------------------------------------------------------------------------
// Path::stroke
// ---------------------------------------------------------------------------

std::optional<Path> Path::stroke(const Stroke& stroke, float resScale) const {
  PathStroker stroker;
  return stroker.stroke(*this, stroke, resScale);
}

}  // namespace tiny_skia
