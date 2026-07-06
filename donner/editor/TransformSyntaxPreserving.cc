#include "donner/editor/TransformSyntaxPreserving.h"

#include <charconv>
#include <cmath>
#include <string>
#include <vector>

#include "donner/base/FormatNumber.h"
#include "donner/base/MathUtils.h"
#include "donner/svg/parser/TransformParser.h"

namespace donner::editor {

namespace {

// Below these thresholds a component is treated as unchanged by the edit, so
// its author-written token is re-emitted verbatim (preserving precision) rather
// than reformatted.
constexpr double kUnchangedEps = 1e-6;
// Deltas below these thresholds mean the edit did not touch that decomposed
// component, so no function of that kind is required to absorb it.
constexpr double kAngleEps = 1e-9;   ///< Radians.
constexpr double kScaleEps = 1e-9;   ///< Unitless scale-ratio deviation from 1.
constexpr double kMatrixEps = 1e-6;  ///< Linear-part identity check for the translate solve.

/// A single parsed number inside a transform function, keeping the author's
/// verbatim token so unchanged parameters round-trip exactly.
struct Arg {
  double value = 0.0;
  std::string text;
};

/// A parsed `name(args...)` transform function.
struct Fn {
  std::string name;
  std::vector<Arg> args;
  bool usesComma = false;  ///< Whether the author separated args with commas.
};

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool IsAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}

/// Parse @p s into an ordered function list, capturing each argument's verbatim
/// token. Returns `std::nullopt` on any malformed input; the caller then falls
/// back to canonical serialization.
std::optional<std::vector<Fn>> ParseFunctionList(std::string_view s) {
  std::vector<Fn> fns;
  std::size_t i = 0;
  const std::size_t n = s.size();
  auto skipSpace = [&] {
    while (i < n && IsSpace(s[i])) {
      ++i;
    }
  };

  skipSpace();
  while (i < n) {
    const std::size_t nameStart = i;
    while (i < n && IsAlpha(s[i])) {
      ++i;
    }
    if (i == nameStart) {
      return std::nullopt;
    }
    Fn fn;
    fn.name = std::string(s.substr(nameStart, i - nameStart));

    skipSpace();
    if (i >= n || s[i] != '(') {
      return std::nullopt;
    }
    ++i;  // consume '('
    skipSpace();

    while (i < n && s[i] != ')') {
      const std::size_t numStart = i;
      if (i < n && (s[i] == '+' || s[i] == '-')) {
        ++i;
      }
      while (i < n && (IsDigit(s[i]) || s[i] == '.')) {
        ++i;
      }
      if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < n && (s[i] == '+' || s[i] == '-')) {
          ++i;
        }
        while (i < n && IsDigit(s[i])) {
          ++i;
        }
      }
      if (i == numStart) {
        return std::nullopt;  // not a number where one was expected
      }

      std::string token(s.substr(numStart, i - numStart));
      double value = 0.0;
      const char* begin = token.c_str();
      char* end = nullptr;
      value = std::strtod(begin, &end);
      if (end != begin + token.size()) {
        return std::nullopt;
      }
      fn.args.push_back(Arg{value, std::move(token)});

      // Consume separators (commas and/or whitespace) between arguments.
      bool sawComma = false;
      while (i < n && (IsSpace(s[i]) || s[i] == ',')) {
        if (s[i] == ',') {
          if (sawComma) {
            return std::nullopt;  // two commas in a row
          }
          sawComma = true;
          fn.usesComma = true;
        }
        ++i;
      }
    }

    if (i >= n) {
      return std::nullopt;  // missing ')'
    }
    ++i;  // consume ')'
    skipSpace();
    fns.push_back(std::move(fn));
  }

  if (fns.empty()) {
    return std::nullopt;
  }
  return fns;
}

std::string FormatEditedNumber(double value) {
  // Kill floating-point noise beyond six decimals (edits never need finer than
  // that) so a computed 60.000000000000004 serializes as "60" rather than a
  // long decimal, while keeping the value well within the verify tolerance.
  double rounded = std::round(value * 1e6) / 1e6;
  if (rounded == 0.0) {
    rounded = 0.0;  // normalize away -0.0
  }
  return detail::FormatNumberForSVG(rounded);
}

/// Assign @p value to argument @p index, keeping the author's verbatim token
/// when the value is unchanged.
void SetArg(Fn& fn, std::size_t index, double value) {
  if (std::abs(value - fn.args[index].value) < kUnchangedEps) {
    return;
  }
  fn.args[index].value = value;
  fn.args[index].text = FormatEditedNumber(value);
}

std::string RenderFn(const Fn& fn) {
  std::string out = fn.name;
  out += '(';
  const char* sep = fn.usesComma ? ", " : " ";
  for (std::size_t k = 0; k < fn.args.size(); ++k) {
    if (k != 0) {
      out += sep;
    }
    out += fn.args[k].text;
  }
  out += ')';
  return out;
}

std::string RenderList(const std::vector<Fn>& fns) {
  std::string out;
  for (std::size_t k = 0; k < fns.size(); ++k) {
    if (k != 0) {
      out += ' ';
    }
    out += RenderFn(fns[k]);
  }
  return out;
}

/// Standard matrix product `A * B` (Donner's `operator*` post-multiplies, so
/// `B * A` yields the conventional product).
Transform2d StandardMul(const Transform2d& a, const Transform2d& b) {
  return b * a;
}

/// Scale-rotate-translate view of a matrix. Mirrors the inspector's
/// DecomposeTransform: returns `std::nullopt` for skewed or singular matrices.
struct Decomposed {
  Vector2d translation;
  double rotationRadians = 0.0;
  Vector2d scale;
};

std::optional<Decomposed> Decompose(const Transform2d& t) {
  const double a = t.data[0];
  const double b = t.data[1];
  const double c = t.data[2];
  const double d = t.data[3];

  const double scaleX = std::hypot(a, b);
  constexpr double kSingularEpsilon = 1e-12;
  if (!(scaleX > kSingularEpsilon) || !std::isfinite(scaleX)) {
    return std::nullopt;
  }

  const double columnDot = a * c + b * d;
  const double columnYNorm = std::hypot(c, d);
  constexpr double kSkewTolerance = 1e-6;
  if (columnYNorm > 0.0 && std::abs(columnDot) > kSkewTolerance * scaleX * columnYNorm) {
    return std::nullopt;
  }

  Decomposed result;
  result.translation = Vector2d(t.data[4], t.data[5]);
  result.rotationRadians = std::atan2(b, a);
  result.scale = Vector2d(scaleX, (a * d - b * c) / scaleX);
  return result;
}

double NormalizeRadians(double r) {
  constexpr double kPi = MathConstants<double>::kPi;
  while (r > kPi) {
    r -= 2.0 * kPi;
  }
  while (r <= -kPi) {
    r += 2.0 * kPi;
  }
  return r;
}

bool TransformsClose(const Transform2d& a, const Transform2d& b) {
  for (int k = 0; k < 6; ++k) {
    const double diff = std::abs(a.data[k] - b.data[k]);
    const double magnitude = std::max(std::abs(a.data[k]), std::abs(b.data[k]));
    if (diff > 1e-3 + 1e-4 * magnitude) {
      return false;
    }
  }
  return true;
}

/// Verify a candidate string re-parses to @p target.
bool Verify(const std::string& candidate, const Transform2d& target) {
  auto parsed = svg::parser::TransformParser::Parse(candidate);
  if (parsed.hasError()) {
    return false;
  }
  return TransformsClose(parsed.result(), target);
}

Transform2d MatrixOf(const std::vector<Fn>& fns) {
  if (fns.empty()) {
    return Transform2d();  // identity
  }
  auto parsed = svg::parser::TransformParser::Parse(RenderList(fns));
  if (parsed.hasError()) {
    return Transform2d();
  }
  return parsed.result();
}

/// Structured translate/scale/rotate update. @p fns is taken by value because
/// it mutates the function tokens. Returns the author-form string on success.
std::optional<RcString> TryStructuredUpdate(std::vector<Fn> fns, const Transform2d& m,
                                            const Transform2d& target) {
  int translateIdx = -1;
  int scaleIdx = -1;
  int rotateIdx = -1;
  int nTranslate = 0;
  int nScale = 0;
  int nRotate = 0;
  for (std::size_t k = 0; k < fns.size(); ++k) {
    const std::string& name = fns[k].name;
    if (name == "translate") {
      ++nTranslate;
      translateIdx = static_cast<int>(k);
    } else if (name == "scale") {
      ++nScale;
      scaleIdx = static_cast<int>(k);
    } else if (name == "rotate") {
      ++nRotate;
      rotateIdx = static_cast<int>(k);
    } else {
      // matrix()/skewX()/skewY()/unknown: not handled by this path.
      return std::nullopt;
    }
  }
  if (nTranslate > 1 || nScale > 1 || nRotate > 1) {
    return std::nullopt;
  }

  const std::optional<Decomposed> dt = Decompose(target);
  const std::optional<Decomposed> dm = Decompose(m);
  if (!dt.has_value() || !dm.has_value()) {
    return std::nullopt;
  }

  const double dScaleX = dt->scale.x / dm->scale.x;
  const double dScaleY = dt->scale.y / dm->scale.y;
  const double dRot = NormalizeRadians(dt->rotationRadians - dm->rotationRadians);

  const bool rotChanged = std::abs(dRot) > kAngleEps;
  const bool scaleChanged =
      std::abs(dScaleX - 1.0) > kScaleEps || std::abs(dScaleY - 1.0) > kScaleEps;

  if (rotChanged && nRotate == 0) {
    return std::nullopt;
  }
  if (scaleChanged && nScale == 0) {
    return std::nullopt;
  }

  if (nRotate == 1) {
    Fn& rf = fns[static_cast<std::size_t>(rotateIdx)];
    if (rf.args.empty() || (rf.args.size() != 1 && rf.args.size() != 3)) {
      return std::nullopt;
    }
    SetArg(rf, 0, rf.args[0].value + dRot * MathConstants<double>::kRadToDeg);
    // Center arguments (if present) are left as-authored; the translate solve
    // and final verify catch any center mismatch.
  }

  if (nScale == 1) {
    Fn& sf = fns[static_cast<std::size_t>(scaleIdx)];
    if (sf.args.size() == 1) {
      const double base = sf.args[0].value;
      const double nx = base * dScaleX;
      const double ny = base * dScaleY;
      if (std::abs(nx - ny) < kUnchangedEps) {
        SetArg(sf, 0, nx);
      } else {
        sf.args[0] = Arg{nx, FormatEditedNumber(nx)};
        sf.args.push_back(Arg{ny, FormatEditedNumber(ny)});
      }
    } else if (sf.args.size() == 2) {
      SetArg(sf, 0, sf.args[0].value * dScaleX);
      SetArg(sf, 1, sf.args[1].value * dScaleY);
    } else {
      return std::nullopt;
    }
  }

  if (nTranslate == 1) {
    const std::vector<Fn> prefix(fns.begin(), fns.begin() + translateIdx);
    const std::vector<Fn> suffix(fns.begin() + translateIdx + 1, fns.end());
    const Transform2d mp = MatrixOf(prefix);
    const Transform2d ms = MatrixOf(suffix);
    // Solve mp * T * ms == target for the pure-translation T.
    const Transform2d tmat = StandardMul(StandardMul(mp.inverse(), target), ms.inverse());
    if (std::abs(tmat.data[0] - 1.0) > kMatrixEps || std::abs(tmat.data[1]) > kMatrixEps ||
        std::abs(tmat.data[2]) > kMatrixEps || std::abs(tmat.data[3] - 1.0) > kMatrixEps) {
      return std::nullopt;
    }
    const double tx = tmat.data[4];
    const double ty = tmat.data[5];
    Fn& tf = fns[static_cast<std::size_t>(translateIdx)];
    if (tf.args.size() == 1) {
      if (std::abs(ty) < kUnchangedEps) {
        SetArg(tf, 0, tx);
      } else {
        tf.args[0] = Arg{tx, FormatEditedNumber(tx)};
        tf.args.push_back(Arg{ty, FormatEditedNumber(ty)});
      }
    } else if (tf.args.size() == 2) {
      SetArg(tf, 0, tx);
      SetArg(tf, 1, ty);
    } else {
      return std::nullopt;
    }
  }

  std::string candidate = RenderList(fns);
  if (Verify(candidate, target)) {
    return RcString(candidate);
  }
  return std::nullopt;
}

/// Rotate-only fallback: express @p target as a rotation, adding explicit center
/// arguments when the rotation is about a non-origin point. Handles the case a
/// rotate-only element is rotated about its bounds center (which the structured
/// path rejects because the resulting matrix carries a translation).
std::optional<RcString> TryRotateOnly(const Fn& rf, const Transform2d& target) {
  if (rf.args.size() != 1 && rf.args.size() != 3) {
    return std::nullopt;
  }
  const std::optional<Decomposed> dt = Decompose(target);
  if (!dt.has_value()) {
    return std::nullopt;
  }
  // Must be a proper rigid rotation: unit scale on both axes, no reflection.
  if (std::abs(dt->scale.x - 1.0) > 1e-6 || std::abs(dt->scale.y - 1.0) > 1e-6) {
    return std::nullopt;
  }

  const double thetaDeg = dt->rotationRadians * MathConstants<double>::kRadToDeg;
  const double tx = target.data[4];
  const double ty = target.data[5];

  Fn out;
  out.name = "rotate";
  out.usesComma = rf.usesComma;

  auto angleToken = [&]() -> Arg {
    if (std::abs(thetaDeg - rf.args[0].value) < kUnchangedEps) {
      return rf.args[0];  // unchanged: keep verbatim
    }
    return Arg{thetaDeg, FormatEditedNumber(thetaDeg)};
  };

  if (std::abs(tx) < kMatrixEps && std::abs(ty) < kMatrixEps) {
    out.args.push_back(angleToken());
  } else {
    const double cos = std::cos(dt->rotationRadians);
    const double sin = std::sin(dt->rotationRadians);
    const double det = (1.0 - cos) * (1.0 - cos) + sin * sin;  // = 2(1 - cos)
    if (std::abs(det) < 1e-12) {
      return std::nullopt;
    }
    // c = (I - R)^{-1} t, with R the standard rotation by theta.
    const double cx = ((1.0 - cos) * tx - sin * ty) / det;
    const double cy = (sin * tx + (1.0 - cos) * ty) / det;

    out.usesComma = true;  // three-argument rotate reads best comma-separated.
    out.args.push_back(angleToken());
    if (rf.args.size() == 3 && std::abs(rf.args[1].value - cx) < kUnchangedEps &&
        std::abs(rf.args[2].value - cy) < kUnchangedEps) {
      out.args.push_back(rf.args[1]);
      out.args.push_back(rf.args[2]);
    } else {
      out.args.push_back(Arg{cx, FormatEditedNumber(cx)});
      out.args.push_back(Arg{cy, FormatEditedNumber(cy)});
    }
  }

  std::string candidate = RenderFn(out);
  if (Verify(candidate, target)) {
    return RcString(candidate);
  }
  return std::nullopt;
}

}  // namespace

std::optional<RcString> reserializeTransformPreservingSyntax(std::string_view authorSource,
                                                             const Transform2d& target) {
  std::optional<std::vector<Fn>> parsed = ParseFunctionList(authorSource);
  if (!parsed.has_value()) {
    return std::nullopt;
  }
  std::vector<Fn>& fns = *parsed;

  auto mResult = svg::parser::TransformParser::Parse(authorSource);
  if (mResult.hasError()) {
    return std::nullopt;
  }
  const Transform2d m = mResult.result();

  // A matrix() author stays matrix(): update the six entries in place.
  if (fns.size() == 1 && fns[0].name == "matrix" && fns[0].args.size() == 6) {
    for (int k = 0; k < 6; ++k) {
      SetArg(fns[0], static_cast<std::size_t>(k), target.data[k]);
    }
    std::string candidate = RenderList(fns);
    if (Verify(candidate, target)) {
      return RcString(candidate);
    }
    return std::nullopt;
  }

  if (auto structured = TryStructuredUpdate(fns, m, target)) {
    return structured;
  }

  // Rotate-only element rotated about a non-origin center: keep rotate() form.
  if (fns.size() == 1 && fns[0].name == "rotate") {
    if (auto rotated = TryRotateOnly(fns[0], target)) {
      return rotated;
    }
  }

  return std::nullopt;
}

RcString serializeTransformForWriteback(const std::optional<RcString>& authorSource,
                                        const Transform2d& target) {
  // Identity clears the attribute; never resurrect an author form for it.
  if (target.isIdentity()) {
    return toSVGTransformString(target);
  }
  if (authorSource.has_value() && !std::string_view(*authorSource).empty()) {
    if (auto preserved =
            reserializeTransformPreservingSyntax(std::string_view(*authorSource), target)) {
      return *preserved;
    }
  }
  return toSVGTransformString(target);
}

}  // namespace donner::editor
