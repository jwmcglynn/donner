#include "donner/svg/core/SVGTransformSerialize.h"

#include <cmath>
#include <cstdio>
#include <vector>

#include "donner/base/MathUtils.h"

namespace donner::svg {

namespace {

void appendCoord(std::vector<char>& buf, double value) {
  char tmp[32];
  int len;
  if (value == std::floor(value) && std::abs(value) < 1e15) {
    len = std::snprintf(tmp, sizeof(tmp), "%lld", static_cast<long long>(value));
  } else {
    len = std::snprintf(tmp, sizeof(tmp), "%g", value);
  }
  buf.insert(buf.end(), tmp, tmp + len);
}

void appendString(std::vector<char>& buf, std::string_view str) {
  buf.insert(buf.end(), str.begin(), str.end());
}

}  // namespace

RcString toSVGTransformString(const Transformd& transform) {
  const double a = transform.data[0];
  const double b = transform.data[1];
  const double c = transform.data[2];
  const double d = transform.data[3];
  const double e = transform.data[4];
  const double f = transform.data[5];

  // Identity check.
  if (NearEquals(a, 1.0, 1e-12) && NearZero(b, 1e-12) && NearZero(c, 1e-12) &&
      NearEquals(d, 1.0, 1e-12) && NearZero(e, 1e-12) && NearZero(f, 1e-12)) {
    return RcString("");
  }

  std::vector<char> buf;
  buf.reserve(64);

  // Pure translation: a=1, b=0, c=0, d=1.
  if (NearEquals(a, 1.0, 1e-12) && NearZero(b, 1e-12) && NearZero(c, 1e-12) &&
      NearEquals(d, 1.0, 1e-12)) {
    appendString(buf, "translate(");
    appendCoord(buf, e);
    buf.push_back(',');
    buf.push_back(' ');
    appendCoord(buf, f);
    buf.push_back(')');
    return RcString::fromVector(std::move(buf));
  }

  // Pure scale: b=0, c=0, e=0, f=0.
  if (NearZero(b, 1e-12) && NearZero(c, 1e-12) && NearZero(e, 1e-12) && NearZero(f, 1e-12)) {
    appendString(buf, "scale(");
    if (NearEquals(a, d, 1e-12)) {
      // Uniform scale.
      appendCoord(buf, a);
    } else {
      appendCoord(buf, a);
      buf.push_back(',');
      buf.push_back(' ');
      appendCoord(buf, d);
    }
    buf.push_back(')');
    return RcString::fromVector(std::move(buf));
  }

  // General matrix.
  appendString(buf, "matrix(");
  appendCoord(buf, a);
  buf.push_back(',');
  buf.push_back(' ');
  appendCoord(buf, b);
  buf.push_back(',');
  buf.push_back(' ');
  appendCoord(buf, c);
  buf.push_back(',');
  buf.push_back(' ');
  appendCoord(buf, d);
  buf.push_back(',');
  buf.push_back(' ');
  appendCoord(buf, e);
  buf.push_back(',');
  buf.push_back(' ');
  appendCoord(buf, f);
  buf.push_back(')');
  return RcString::fromVector(std::move(buf));
}

}  // namespace donner::svg
