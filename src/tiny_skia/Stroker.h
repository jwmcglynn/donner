#pragma once

#include <optional>

#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Scalar.h"
#include "tiny_skia/Stroke.h"

namespace tiny_skia {

enum class StrokeType : std::int8_t {
  Outer = 1,
  Inner = -1,
};

enum class ReductionType : std::uint8_t {
  Point,
  Line,
  Quad,
  Degenerate,
  Degenerate2,
  Degenerate3,
};

enum class ResultType : std::uint8_t {
  Split,
  Degenerate,
  Quad,
};

enum class IntersectRayType : std::uint8_t {
  CtrlPt,
  Result,
};

enum class AngleType : std::uint8_t {
  Nearly180,
  Sharp,
  Shallow,
  NearlyLine,
};

struct SwappableBuilders {
  PathBuilder* inner;
  PathBuilder* outer;
  void swap() { std::swap(inner, outer); }
};

struct QuadConstruct {
  Point quad[3] = {};
  Point tangentStart = Point::zero();
  Point tangentEnd = Point::zero();
  NormalizedF32 startT = NormalizedF32::ZERO;
  NormalizedF32 midT = NormalizedF32::ZERO;
  NormalizedF32 endT = NormalizedF32::ZERO;
  bool startSet = false;
  bool endSet = false;
  bool oppositeTangents = false;

  bool init(NormalizedF32 start, NormalizedF32 end);
  bool initWithStart(const QuadConstruct& parent);
  bool initWithEnd(const QuadConstruct& parent);
};

using CapProc = void (*)(Point pivot, Point normal, Point stop, const PathBuilder* otherPath,
                         PathBuilder& path);

using JoinProc = void (*)(Point beforeUnitNormal, Point pivot, Point afterUnitNormal, float radius,
                          float invMiterLimit, bool prevIsLine, bool currIsLine,
                          SwappableBuilders builders);

/// A path stroker.
class PathStroker {
 public:
  PathStroker();

  static float computeResolutionScale(const Transform& ts);

  std::optional<Path> stroke(const Path& path, const Stroke& stroke, float resolutionScale);

 private:
  std::optional<Path> strokeInner(const Path& path, float width, float miterLimit, LineCap lineCap,
                                  LineJoin lineJoin, float resScale);

  SwappableBuilders builders();
  Point moveToPt() const;

  void moveTo(Point p);
  void lineTo(Point p, const PathSegmentsIter* iter);
  void quadTo(Point p1, Point p2);
  void cubicTo(Point pt1, Point pt2, Point pt3);
  bool cubicStroke(const Point cubic[4], QuadConstruct& quadPoints);
  bool cubicMidOnLine(const Point cubic[4], QuadConstruct& quadPoints);
  void cubicQuadMid(const Point cubic[4], QuadConstruct& quadPoints, Point& mid);
  void cubicPerpRay(const Point cubic[4], NormalizedF32 t, Point& tPt, Point& onPt, Point* tangent);
  void setCubicEndNormal(const Point cubic[4], Point normalAb, Point unitNormalAb, Point& normalCd,
                         Point& unitNormalCd);
  ResultType compareQuadCubic(const Point cubic[4], QuadConstruct& quadPoints);
  void cubicQuadEnds(const Point cubic[4], QuadConstruct& quadPoints);

  void closeContour(bool isLine);
  void finishContour(bool close, bool currIsLine);

  bool preJoinTo(Point p, bool currIsLine, Point& normal, Point& unitNormal);
  void postJoinTo(Point p, Point normal, Point unitNormal);

  void initQuad(StrokeType strokeType, NormalizedF32 start, NormalizedF32 end,
                QuadConstruct& quadPoints);
  bool quadStroke(const Point quad[3], QuadConstruct& quadPoints);
  ResultType compareQuadQuad(const Point quad[3], QuadConstruct& quadPoints);

  void setRayPoints(Point tp, Point& dxy, Point& onP, Point* tangent);
  void quadPerpRay(const Point quad[3], NormalizedF32 t, Point& tp, Point& onP, Point* tangent);
  void addDegenerateLine(const QuadConstruct& quadPoints);

  ResultType strokeCloseEnough(const Point stroke[3], const Point ray[2],
                               QuadConstruct& quadPoints);
  ResultType intersectRay(IntersectRayType type, QuadConstruct& quadPoints);
  ResultType tangentsMeet(const Point cubic[4], QuadConstruct& quadPoints);

  std::optional<Path> finish(bool isLine);
  bool hasOnlyMoveTo() const;
  bool isCurrentContourEmpty() const;

  float radius_ = 0.0f;
  float invMiterLimit_ = 0.0f;
  float resScale_ = 1.0f;
  float invResScale_ = 1.0f;
  float invResScaleSquared_ = 1.0f;

  Point firstNormal_ = Point::zero();
  Point prevNormal_ = Point::zero();
  Point firstUnitNormal_ = Point::zero();
  Point prevUnitNormal_ = Point::zero();

  Point firstPt_ = Point::zero();
  Point prevPt_ = Point::zero();

  Point firstOuterPt_ = Point::zero();
  std::size_t firstOuterPtIndexInContour_ = 0;
  std::int32_t segmentCount_ = -1;
  bool prevIsLine_ = false;

  CapProc capper_ = nullptr;
  JoinProc joiner_ = nullptr;

  PathBuilder inner_;
  PathBuilder outer_;
  PathBuilder cusper_;

  StrokeType strokeType_ = StrokeType::Outer;

  std::int32_t recursionDepth_ = 0;
  bool foundTangents_ = false;
  bool joinCompleted_ = false;
};

// Free functions used by the stroker.
bool setNormalUnitNormal(Point before, Point after, float scale, float radius, Point& normal,
                         Point& unitNormal);
bool setNormalUnitNormal2(Point vec, float radius, Point& normal, Point& unitNormal);
bool isClockwise(Point before, Point after);
AngleType dotToAngleType(float dot);
void handleInnerJoin(Point pivot, Point after, PathBuilder& inner);

std::pair<Point, ReductionType> checkQuadLinear(const Point quad[3]);
ReductionType checkCubicLinear(const Point cubic[4], Point reduction[3], Point* tangentPt);
bool degenerateVector(Point v);
bool quadInLine(const Point quad[3]);
bool cubicInLine(const Point cubic[4]);

float ptToLine(Point pt, Point lineStart, Point lineEnd);
std::size_t intersectQuadRay(const Point line[2], const Point quad[3],
                             NormalizedF32Exclusive roots[3]);
bool pointsWithinDist(Point nearPt, Point farPt, float limit);
bool sharpAngle(const Point quad[3]);
bool ptInQuadBounds(const Point quad[3], Point pt, float invResScale);

// Cap/join functions.
void buttCapper(Point pivot, Point normal, Point stop, const PathBuilder* otherPath,
                PathBuilder& path);
void roundCapper(Point pivot, Point normal, Point stop, const PathBuilder* otherPath,
                 PathBuilder& path);
void squareCapper(Point pivot, Point normal, Point stop, const PathBuilder* otherPath,
                  PathBuilder& path);

void bevelJoiner(Point beforeUnitNormal, Point pivot, Point afterUnitNormal, float radius,
                 float invMiterLimit, bool prevIsLine, bool currIsLine, SwappableBuilders builders);
void roundJoiner(Point beforeUnitNormal, Point pivot, Point afterUnitNormal, float radius,
                 float invMiterLimit, bool prevIsLine, bool currIsLine, SwappableBuilders builders);
void miterJoiner(Point beforeUnitNormal, Point pivot, Point afterUnitNormal, float radius,
                 float invMiterLimit, bool prevIsLine, bool currIsLine, SwappableBuilders builders);
void miterClipJoiner(Point beforeUnitNormal, Point pivot, Point afterUnitNormal, float radius,
                     float invMiterLimit, bool prevIsLine, bool currIsLine,
                     SwappableBuilders builders);

CapProc capFactory(LineCap cap);
JoinProc joinFactory(LineJoin join);

}  // namespace tiny_skia
