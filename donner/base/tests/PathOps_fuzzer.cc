#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "donner/base/PathOps.h"

namespace donner {
namespace {

bool ReadByte(const std::uint8_t*& cursor, std::size_t& remaining, std::uint8_t* out) {
  if (remaining == 0u) {
    return false;
  }

  *out = *cursor;
  ++cursor;
  --remaining;
  return true;
}

bool ReadCoord(const std::uint8_t*& cursor, std::size_t& remaining, double* out) {
  if (remaining < 2u) {
    return false;
  }

  const std::uint16_t raw =
      (static_cast<std::uint16_t>(cursor[0]) << 8u) | static_cast<std::uint16_t>(cursor[1]);
  cursor += 2u;
  remaining -= 2u;
  *out = (static_cast<double>(raw) / 65535.0) * 200.0 - 100.0;
  return true;
}

bool ReadPoint(const std::uint8_t*& cursor, std::size_t& remaining, Vector2d* out) {
  return ReadCoord(cursor, remaining, &out->x) && ReadCoord(cursor, remaining, &out->y);
}

Path ReadClosedPath(const std::uint8_t*& cursor, std::size_t& remaining) {
  PathBuilder builder;
  Vector2d start;
  if (!ReadPoint(cursor, remaining, &start)) {
    return {};
  }

  builder.moveTo(start);
  std::uint8_t commandBudgetByte = 0;
  if (!ReadByte(cursor, remaining, &commandBudgetByte)) {
    builder.closePath();
    return builder.build();
  }

  const std::uint8_t commandCount = static_cast<std::uint8_t>(1u + commandBudgetByte % 12u);
  for (std::uint8_t i = 0; i < commandCount; ++i) {
    std::uint8_t verbByte = 0;
    if (!ReadByte(cursor, remaining, &verbByte)) {
      break;
    }

    switch (verbByte % 3u) {
      case 0: {
        Vector2d point;
        if (!ReadPoint(cursor, remaining, &point)) {
          i = commandCount;
          break;
        }
        builder.lineTo(point);
        break;
      }
      case 1: {
        Vector2d control;
        Vector2d end;
        if (!ReadPoint(cursor, remaining, &control) || !ReadPoint(cursor, remaining, &end)) {
          i = commandCount;
          break;
        }
        builder.quadTo(control, end);
        break;
      }
      case 2: {
        Vector2d c1;
        Vector2d c2;
        Vector2d end;
        if (!ReadPoint(cursor, remaining, &c1) || !ReadPoint(cursor, remaining, &c2) ||
            !ReadPoint(cursor, remaining, &end)) {
          i = commandCount;
          break;
        }
        builder.curveTo(c1, c2, end);
        break;
      }
    }
  }

  builder.closePath();
  return builder.build();
}

PathBooleanOp OpFromByte(std::uint8_t byte) {
  switch (byte % 4u) {
    case 0: return PathBooleanOp::Union;
    case 1: return PathBooleanOp::Intersect;
    case 2: return PathBooleanOp::Difference;
    case 3: return PathBooleanOp::Xor;
  }
  return PathBooleanOp::Union;
}

FillRule FillRuleFromByte(std::uint8_t byte) {
  return (byte & 1u) == 0u ? FillRule::NonZero : FillRule::EvenOdd;
}

Transform2d TransformFromByte(std::uint8_t byte) {
  switch (byte % 5u) {
    case 0: return Transform2d();
    case 1: return Transform2d::Translate({12.5, -7.5});
    case 2: return Transform2d::Scale({1.5, 0.75});
    case 3: return Transform2d::Rotate(0.25);
    case 4: return Transform2d::SkewX(0.2);
  }
  return Transform2d();
}

bool PathHasFinitePoints(const Path& path) {
  for (const Vector2d& point : path.points()) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
      return false;
    }
  }
  return true;
}

}  // namespace

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::uint8_t* cursor = data;
  std::size_t remaining = size;

  std::uint8_t opByte = 0;
  std::uint8_t lhsFillByte = 0;
  std::uint8_t rhsFillByte = 0;
  std::uint8_t lhsTransformByte = 0;
  std::uint8_t rhsTransformByte = 0;
  if (!ReadByte(cursor, remaining, &opByte) || !ReadByte(cursor, remaining, &lhsFillByte) ||
      !ReadByte(cursor, remaining, &rhsFillByte) ||
      !ReadByte(cursor, remaining, &lhsTransformByte) ||
      !ReadByte(cursor, remaining, &rhsTransformByte)) {
    return 0;
  }

  Path lhs = ReadClosedPath(cursor, remaining);
  Path rhs = ReadClosedPath(cursor, remaining);
  if (lhs.empty() || rhs.empty()) {
    return 0;
  }

  const PathBooleanInput inputs[2] = {
      PathBooleanInput{
          .path = std::move(lhs),
          .fillRule = FillRuleFromByte(lhsFillByte),
          .outputFromPath = TransformFromByte(lhsTransformByte),
      },
      PathBooleanInput{
          .path = std::move(rhs),
          .fillRule = FillRuleFromByte(rhsFillByte),
          .outputFromPath = TransformFromByte(rhsTransformByte),
      },
  };

  PathBooleanOptions options;
  options.maxCurveCount = 64;
  options.maxIntersections = 256;
  options.maxOutputCommands = 512;

  const PathBooleanResult result = ApplyPathBoolean(OpFromByte(opByte), inputs, options);
  if (result.status == PathBooleanStatus::Ok) {
    assert(!result.paths.empty() && "Ok result must include output paths");
    for (const Path& path : result.paths) {
      assert(path.verbCount() <= options.maxOutputCommands);
      assert(PathHasFinitePoints(path));
    }
  } else {
    assert(result.paths.empty() && "Non-Ok result must not expose partial output paths");
  }

  return 0;
}

}  // namespace donner
