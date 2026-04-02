// Copyright 2014 Google Inc.
// Copyright 2020 Yevhenii Reizner
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This module is a C++ port of SkDashPath, SkDashPathEffect, SkContourMeasure
// and SkPathMeasure from the Rust tiny-skia dash.rs.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Scalar.h"
#include "tiny_skia/Stroke.h"

namespace tiny_skia {

/// The type of a contour segment.
enum class SegmentType : std::uint8_t {
  Line,
  Quad,
  Cubic,
};

/// A single measured segment within a contour.
struct Segment {
  float distance = 0.0f;       // total distance up to this point
  std::size_t pointIndex = 0;  // index into the ContourMeasure::points array
  std::uint32_t tValue = 0;
  SegmentType kind = SegmentType::Line;

  /// Convert the integer t-value to a floating-point scalar in [0,1].
  [[nodiscard]] float scalarT() const;
};

/// Internal precomputed dash parameters derived from a StrokeDash.
struct DashParams {
  float intervalLen = 0.0f;
  float firstLen = 0.0f;
  std::size_t firstIndex = 0;
  float adjustedOffset = 0.0f;

  /// Compute dash parameters from a StrokeDash.
  /// Returns std::nullopt if the dash is invalid (e.g. zero interval length).
  [[nodiscard]] static std::optional<DashParams> create(const StrokeDash& dash);
};

/// Measurement of a single contour (sub-path).
/// Stores flattened segment info and original points for later extraction.
struct ContourMeasure {
  std::vector<Segment> segments;
  std::vector<Point> points;
  float length = 0.0f;
  bool isClosed = false;

  /// Push a sub-range of this contour [startD, stopD] into a PathBuilder.
  void pushSegment(float startD, float stopD, bool startWithMoveTo, PathBuilder& pb) const;

  /// Map a distance value to a (segmentIndex, NormalizedF32 t) pair.
  [[nodiscard]] std::optional<std::pair<std::size_t, NormalizedF32>> distanceToSegment(
      float distance) const;

  /// Measure a line from p0 to p1, appending a segment if the distance grows.
  float computeLineSeg(Point p0, Point p1, float distance, std::size_t pointIndex);

  /// Recursively measure a quadratic, subdividing until flat enough.
  float computeQuadSegs(Point p0, Point p1, Point p2, float distance, std::uint32_t minT,
                        std::uint32_t maxT, std::size_t pointIndex, float tolerance);

  /// Recursively measure a cubic, subdividing until flat enough.
  float computeCubicSegs(Point p0, Point p1, Point p2, Point p3, float distance, std::uint32_t minT,
                         std::uint32_t maxT, std::size_t pointIndex, float tolerance);
};

/// Iterator that yields ContourMeasure for each contour in a path.
class ContourMeasureIter {
 public:
  ContourMeasureIter(const Path& path, float resScale);

  /// Yield the next measured contour, skipping zero-length contours.
  std::optional<ContourMeasure> next();

 private:
  PathSegmentsIter iter_;
  float tolerance_;
};

/// Apply the dash pattern to src, returning a new dashed path.
/// Returns std::nullopt when more than 1,000,000 dashes would be produced
/// or when the final path has an invalid bounding box.
[[nodiscard]] std::optional<Path> dashImpl(const Path& src, const StrokeDash& dash, float resScale);

}  // namespace tiny_skia
