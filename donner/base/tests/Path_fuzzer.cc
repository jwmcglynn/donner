#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

#include "donner/base/Path.h"

namespace donner {

namespace {

/// Read a double from the fuzzer data buffer, advancing the pointer.
/// Returns false if not enough data remains.
bool readDouble(const uint8_t*& data, size_t& remaining, double& out) {
  if (remaining < sizeof(double)) {
    return false;
  }
  std::memcpy(&out, data, sizeof(double));
  data += sizeof(double);
  remaining -= sizeof(double);
  return true;
}

/// Read a Vector2d (two doubles) from the fuzzer data buffer.
bool readVector2d(const uint8_t*& data, size_t& remaining, Vector2d& out) {
  return readDouble(data, remaining, out.x) && readDouble(data, remaining, out.y);
}

/// Returns true if the value is finite (not NaN and not Inf).
bool isFinite(double v) {
  return std::isfinite(v);
}

/// Returns true if a Vector2d has finite components.
bool isFiniteVec(const Vector2d& v) {
  return isFinite(v.x) && isFinite(v.y);
}

}  // namespace

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  constexpr std::string_view kExtremeRoundStrokeSeed = "EXTREME_ROUND_STROKE\n";
  if (size == kExtremeRoundStrokeSeed.size() &&
      std::memcmp(data, kExtremeRoundStrokeSeed.data(), size) == 0) {
    PathBuilder seedBuilder;
    seedBuilder.moveTo(Vector2d(0.0, 0.0));
    seedBuilder.lineTo(Vector2d(1.0, 0.0));
    const StrokeStyle style{
        .width = 1.0e300,
        .cap = LineCap::Round,
        .join = LineJoin::Round,
    };
    (void)seedBuilder.build().strokeToFill(style, 0.25);
    return 0;
  }

  constexpr std::string_view kExtremeDashedRoundStrokeSeed = "EXTREME_DASHED_ROUND_STROKE\n";
  if (size == kExtremeDashedRoundStrokeSeed.size() &&
      std::memcmp(data, kExtremeDashedRoundStrokeSeed.data(), size) == 0) {
    const Path path = PathBuilder().moveTo({0.0, 0.0}).lineTo({1.0, 0.0}).build();
    const StrokeStyle style{
        .width = 1.0e300,
        .cap = LineCap::Round,
        .join = LineJoin::Round,
        .dashArray = {0.0, 0.0001},
    };
    assert(path.strokeToFill(style, 0.25).empty());
    return 0;
  }

  constexpr std::string_view kExtremeCurveStrokeSeed = "EXTREME_CURVE_STROKE\n";
  if (size == kExtremeCurveStrokeSeed.size() &&
      std::memcmp(data, kExtremeCurveStrokeSeed.data(), size) == 0) {
    PathBuilder seedBuilder;
    seedBuilder.moveTo({0.0, 0.0});
    for (int i = 0; i < 2048; ++i) {
      const double x = static_cast<double>(i);
      seedBuilder.curveTo({x + 0.25, 1.0e12}, {x + 0.75, -1.0e12}, {x + 1.0, 0.0});
    }
    const StrokeStyle style{.width = 10.0, .cap = LineCap::Round, .join = LineJoin::Round};
    assert(seedBuilder.build().strokeToFill(style, 0.0).empty());
    return 0;
  }

  constexpr std::string_view kDashPatternProgressWorkSeed = "DASH_PATTERN_PROGRESS_WORK\n";
  if (size == kDashPatternProgressWorkSeed.size() &&
      std::memcmp(data, kDashPatternProgressWorkSeed.data(), size) == 0) {
    const Path tinyPath = PathBuilder().moveTo({0.0, 0.0}).lineTo({1.0, 0.0}).build();
    const StrokeStyle tinyProgressStyle{
        .width = 1.0,
        .dashArray = {0.0, std::numeric_limits<double>::denorm_min()},
    };
    assert(tinyPath.strokeToFill(tinyProgressStyle, 0.25).empty());

    PathBuilder aggregateBuilder;
    for (std::size_t i = 0; i < 3; ++i) {
      const double x = static_cast<double>(i) * 2.0;
      aggregateBuilder.moveTo({x, 0.0}).lineTo({x + 0.5, 0.0});
    }
    StrokeStyle aggregateStyle;
    aggregateStyle.width = 1.0;
    for (std::size_t i = 0; i < 127; ++i) {
      aggregateStyle.dashArray.insert(aggregateStyle.dashArray.end(), {0.0, 1.0e-8});
    }
    aggregateStyle.dashArray.insert(aggregateStyle.dashArray.end(), {0.001, 0.0});
    assert(aggregateBuilder.build().strokeToFill(aggregateStyle, 0.25).empty());

    const Path roundedPath = PathBuilder().moveTo({0.0, 0.0}).lineTo({2.0e16, 0.0}).build();
    const StrokeStyle roundedProgressStyle{
        .width = 1.0,
        .dashArray = {1.0e16, 1.0},
    };
    assert(roundedPath.strokeToFill(roundedProgressStyle, 0.25).empty());
    return 0;
  }

  constexpr std::string_view kDashPolylineIndexWorkSeed = "DASH_POLYLINE_INDEX_WORK\n";
  if (size == kDashPolylineIndexWorkSeed.size() &&
      std::memcmp(data, kDashPolylineIndexWorkSeed.data(), size) == 0) {
    PathBuilder builder;
    builder.moveTo({0, 0});
    for (std::size_t i = 1; i <= 32767; ++i) {
      builder.lineTo({static_cast<double>(i), 0.0});
    }
    const StrokeStyle style{.width = 1.0, .dashArray = {2.0, 2.0}};
    assert(!builder.build().strokeToFill(style, 0.25).empty());
    return 0;
  }

  constexpr std::string_view kExtremeArcSeed = "EXTREME_ARC\n";
  if (size == kExtremeArcSeed.size() && std::memcmp(data, kExtremeArcSeed.data(), size) == 0) {
    const Path path = PathBuilder()
                          .moveTo({0.0, 0.0})
                          .arcTo({1e308, 1e308}, 0.0, false, false, {1e308, 1e308})
                          .build();
    assert(path.verbCount() == 2);
    return 0;
  }

  constexpr std::string_view kPathMeasurementSeed = "PATH_MEASUREMENT_MANY_SUBPATHS\n";
  if (size == kPathMeasurementSeed.size() &&
      std::memcmp(data, kPathMeasurementSeed.data(), size) == 0) {
    PathBuilder builder;
    constexpr std::size_t kSubpathCount = 16384;
    for (std::size_t i = 0; i < kSubpathCount; ++i) {
      builder.moveTo({static_cast<double>(i), 0.0}).closePath();
    }
    const Path path = builder.build();
    const Path::MeasuredPath measured = path.measure();
    assert(measured.segmentCount() == kSubpathCount);
    assert(path.pathLength() == 0.0);
    for (std::size_t i = 0; i < 4096; ++i) {
      assert(measured.pointAtArcLength(0.0).valid);
    }
    return 0;
  }

  constexpr std::string_view kRecursiveGeometryWorkSeed = "PATH_RECURSIVE_GEOMETRY_WORK\n";
  if (size == kRecursiveGeometryWorkSeed.size() &&
      std::memcmp(data, kRecursiveGeometryWorkSeed.data(), size) == 0) {
    const Path samplePath =
        PathBuilder()
            .moveTo({1000.0, 0.0})
            .curveTo({1000.0, 552.2847498}, {552.2847498, 1000.0}, {0.0, 1000.0})
            .build();
    const Path::MeasuredPath sampleMeasurement = samplePath.measure();
    assert(sampleMeasurement.valid());
    for (std::size_t i = 0; i < 4096; ++i) {
      assert(sampleMeasurement.pointAtArcLength(sampleMeasurement.pathLength() * 0.5).valid);
    }

    PathBuilder builder;
    for (std::size_t i = 0; i < 512; ++i) {
      builder.moveTo({1000.0, 0.0})
          .curveTo({1000.0, 552.2847498}, {552.2847498, 1000.0}, {0.0, 1000.0});
    }
    const Path path = builder.build();
    const Path::MeasuredPath measured = path.measure();
    assert(!measured.valid());
    assert(measured.measurementWorkUnits() == Path::kMaximumGeometryQueryWork);
    assert(measured.segmentCount() == 0);
    assert(std::isinf(path.pathLength()));
    assert(!path.isInside({2000.0, 2000.0}));
    assert(!path.isOnPath({2000.0, 2000.0}, 0.001));

    const Path slowStart = PathBuilder().moveTo({0, 0}).curveTo({0, 0}, {0, 0}, {10, 0}).build();
    const Path slowEnd = PathBuilder().moveTo({0, 0}).curveTo({10, 0}, {10, 0}, {10, 0}).build();
    assert(std::abs(slowStart.measure().pointAtArcLength(5.0).point.x - 5.0) <= 0.001);
    assert(std::abs(slowEnd.measure().pointAtArcLength(5.0).point.x - 5.0) <= 0.001);
    return 0;
  }

  constexpr std::string_view kSubdivisionDepthLimitSeed = "PATH_SUBDIVISION_DEPTH_LIMIT\n";
  if (size == kSubdivisionDepthLimitSeed.size() &&
      std::memcmp(data, kSubdivisionDepthLimitSeed.data(), size) == 0) {
    const Path path = PathBuilder()
                          .moveTo({0.0, 0.0})
                          .curveTo({1e100, 1e100}, {-1e100, 1e100}, {0.0, 0.0})
                          .build();
    const Path::MeasuredPath measured = path.measure();
    assert(!measured.valid());
    assert(measured.measurementWorkUnits() ==
           path.verbCount() +
               static_cast<std::size_t>(Path::kMaximumArcLengthSubdivisionDepth + 1));
    assert(std::isinf(path.pathLength()));
    assert(!path.isOnPath({0.0, 7.5e99}, 0.001));
    assert(!path.isInside({0.0, 7.5e99}));
    const Path flattened = path.flatten(0.001);
    assert(!flattened.empty());
    assert(flattened.verbCount() <=
           1u + (1u << static_cast<unsigned int>(Path::kMaximumFlattenSubdivisionDepth)));
    assert(!path.strokeToFill({.width = 1.0}, 0.001).empty());
    return 0;
  }

  const uint8_t* cursor = data;
  size_t remaining = size;
  double strokeWidth = 1.0;
  if (size >= sizeof(double)) {
    std::memcpy(&strokeWidth, data, sizeof(double));
  }

  // Interpret input as a stream of path commands.
  // Each command starts with a byte selecting the verb:
  //   0 = moveTo (consumes 1 Vector2d = 16 bytes)
  //   1 = lineTo (consumes 1 Vector2d = 16 bytes)
  //   2 = quadTo (consumes 2 Vector2d = 32 bytes)
  //   3 = curveTo (consumes 3 Vector2d = 48 bytes)
  //   4 = arcTo (consumes radius, rotation, and end = 40 bytes)
  //   5 = closePath (consumes 0 bytes)
  // Cap at 256 commands to avoid excessive memory usage.

  PathBuilder builder;
  int commandCount = 0;
  constexpr int kMaxCommands = 256;
  bool hadCommand = false;

  while (remaining > 0 && commandCount < kMaxCommands) {
    const uint8_t verbByte = *cursor;
    ++cursor;
    --remaining;

    const uint8_t verb = verbByte % 6;

    switch (verb) {
      case 0: {  // moveTo
        Vector2d pt;
        if (!readVector2d(cursor, remaining, pt)) {
          goto done;
        }
        if (!isFiniteVec(pt)) {
          break;
        }
        builder.moveTo(pt);
        hadCommand = true;
        break;
      }
      case 1: {  // lineTo
        Vector2d pt;
        if (!readVector2d(cursor, remaining, pt)) {
          goto done;
        }
        if (!isFiniteVec(pt)) {
          break;
        }
        builder.lineTo(pt);
        hadCommand = true;
        break;
      }
      case 2: {  // quadTo
        Vector2d control, end;
        if (!readVector2d(cursor, remaining, control) || !readVector2d(cursor, remaining, end)) {
          goto done;
        }
        if (!isFiniteVec(control) || !isFiniteVec(end)) {
          break;
        }
        builder.quadTo(control, end);
        hadCommand = true;
        break;
      }
      case 3: {  // curveTo
        Vector2d c1, c2, end;
        if (!readVector2d(cursor, remaining, c1) || !readVector2d(cursor, remaining, c2) ||
            !readVector2d(cursor, remaining, end)) {
          goto done;
        }
        if (!isFiniteVec(c1) || !isFiniteVec(c2) || !isFiniteVec(end)) {
          break;
        }
        builder.curveTo(c1, c2, end);
        hadCommand = true;
        break;
      }
      case 4: {  // arcTo
        Vector2d radius, end;
        double rotation = 0.0;
        if (!readVector2d(cursor, remaining, radius) || !readDouble(cursor, remaining, rotation) ||
            !readVector2d(cursor, remaining, end)) {
          goto done;
        }
        if (!isFiniteVec(radius) || !isFinite(rotation) || !isFiniteVec(end)) {
          break;
        }
        builder.arcTo(radius, rotation, (verbByte & 0x40) != 0, (verbByte & 0x80) != 0, end);
        hadCommand = true;
        break;
      }
      case 5: {  // closePath
        builder.closePath();
        hadCommand = true;
        break;
      }
      default: break;
    }

    ++commandCount;
  }

done:
  if (!hadCommand) {
    return 0;
  }

  Path path = builder.build();

  // The path may be empty if all commands were no-ops (e.g., closePath without moveTo).
  if (path.empty()) {
    return 0;
  }

  // --- cubicToQuadratic ---
  {
    Path quadPath = path.cubicToQuadratic();

    // Verify no CurveTo verbs remain.
    quadPath.forEach([](Path::Verb verb, std::span<const Vector2d> /*points*/) {
      assert(verb != Path::Verb::CurveTo && "cubicToQuadratic should eliminate all CurveTo verbs");
    });
  }

  // --- toMonotonic ---
  {
    Path monoPath = path.toMonotonic();
    assert(!monoPath.empty() && "toMonotonic result should not be empty");
  }

  // --- flatten ---
  {
    Path flatPath = path.flatten();
    assert(!flatPath.empty() && "flatten result should not be empty");

    // Verify only MoveTo, LineTo, ClosePath remain.
    flatPath.forEach([](Path::Verb verb, std::span<const Vector2d> /*points*/) {
      assert(verb != Path::Verb::QuadTo && verb != Path::Verb::CurveTo &&
             "flatten should eliminate all curve verbs");
    });
  }

  // --- bounds (should not crash) ---
  {
    Box2d box = path.bounds();
    (void)box;
  }

  // --- measured arc length and repeated sampling ---
  {
    const Path::MeasuredPath measured = path.measure();
    assert(measured.segmentCount() <= path.verbCount());
    const double length = measured.pathLength();
    (void)path.pathLength();
    if (std::isfinite(length) && length >= 0.0) {
      (void)measured.pointAtArcLength(length * 0.5);
      (void)path.pointAtArcLength(length * 0.5);
    }
  }

  // --- strokeToFill ---
  {
    const StrokeStyle style{
        .width = strokeWidth,
        .cap = LineCap::Round,
        .join = LineJoin::Round,
    };
    (void)path.strokeToFill(style, 0.25);
  }

  return 0;
}

}  // namespace donner
