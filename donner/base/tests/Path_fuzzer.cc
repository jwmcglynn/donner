#include <cassert>
#include <cmath>
#include <cstring>

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
  const uint8_t* cursor = data;
  size_t remaining = size;

  // Interpret input as a stream of path commands.
  // Each command starts with a byte selecting the verb:
  //   0 = moveTo (consumes 1 Vector2d = 16 bytes)
  //   1 = lineTo (consumes 1 Vector2d = 16 bytes)
  //   2 = quadTo (consumes 2 Vector2d = 32 bytes)
  //   3 = curveTo (consumes 3 Vector2d = 48 bytes)
  //   4 = closePath (consumes 0 bytes)
  // Cap at 256 commands to avoid excessive memory usage.

  PathBuilder builder;
  int commandCount = 0;
  constexpr int kMaxCommands = 256;
  bool hadCommand = false;

  while (remaining > 0 && commandCount < kMaxCommands) {
    const uint8_t verbByte = *cursor;
    ++cursor;
    --remaining;

    const uint8_t verb = verbByte % 5;

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
      case 4: {  // closePath
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

  // The path should not be empty if we added commands.
  assert(!path.empty() && "Path should not be empty after adding commands");

  // --- cubicToQuadratic ---
  {
    Path quadPath = path.cubicToQuadratic();
    // If the original path had commands, the quadratic version should too.
    assert(!quadPath.empty() && "cubicToQuadratic result should not be empty");

    // Verify no CurveTo verbs remain.
    quadPath.forEach([](Path::Verb verb, std::span<const Vector2d> /*points*/) {
      assert(verb != Path::Verb::CurveTo &&
             "cubicToQuadratic should eliminate all CurveTo verbs");
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

  return 0;
}

}  // namespace donner
