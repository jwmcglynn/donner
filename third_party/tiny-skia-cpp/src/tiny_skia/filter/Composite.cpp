#include "tiny_skia/filter/Composite.h"

#include <algorithm>
#include <cstddef>

namespace tiny_skia::filter {

void composite(const Pixmap& in1, const Pixmap& in2, Pixmap& dst, CompositeOp op, double k1,
               double k2, double k3, double k4) {
  const auto src1 = in1.data();
  const auto src2 = in2.data();
  auto out = dst.data();

  const std::size_t pixelCount = std::min({src1.size(), src2.size(), out.size()}) / 4;

  for (std::size_t i = 0; i < pixelCount; ++i) {
    const std::size_t off = i * 4;

    // Premultiplied RGBA values normalized to [0, 1].
    const double r1 = src1[off + 0] / 255.0;
    const double g1 = src1[off + 1] / 255.0;
    const double b1 = src1[off + 2] / 255.0;
    const double a1 = src1[off + 3] / 255.0;

    const double r2 = src2[off + 0] / 255.0;
    const double g2 = src2[off + 1] / 255.0;
    const double b2 = src2[off + 2] / 255.0;
    const double a2 = src2[off + 3] / 255.0;

    double ro = 0.0;
    double go = 0.0;
    double bo = 0.0;
    double ao = 0.0;

    // Porter-Duff compositing on premultiplied values:
    // Co = Fa * C1 + Fb * C2
    // Ao = Fa * A1 + Fb * A2
    auto porterDuff = [&](double fa, double fb) {
      ro = fa * r1 + fb * r2;
      go = fa * g1 + fb * g2;
      bo = fa * b1 + fb * b2;
      ao = fa * a1 + fb * a2;
    };

    switch (op) {
      case CompositeOp::Over: porterDuff(1.0, 1.0 - a1); break;
      case CompositeOp::In: porterDuff(a2, 0.0); break;
      case CompositeOp::Out: porterDuff(1.0 - a2, 0.0); break;
      case CompositeOp::Atop: porterDuff(a2, 1.0 - a1); break;
      case CompositeOp::Xor: porterDuff(1.0 - a2, 1.0 - a1); break;
      case CompositeOp::Lighter: porterDuff(1.0, 1.0); break;
      case CompositeOp::Arithmetic:
        ro = k1 * r1 * r2 + k2 * r1 + k3 * r2 + k4;
        go = k1 * g1 * g2 + k2 * g1 + k3 * g2 + k4;
        bo = k1 * b1 * b2 + k2 * b1 + k3 * b2 + k4;
        ao = k1 * a1 * a2 + k2 * a1 + k3 * a2 + k4;
        break;
    }

    out[off + 0] = static_cast<std::uint8_t>(std::clamp(ro * 255.0, 0.0, 255.0));
    out[off + 1] = static_cast<std::uint8_t>(std::clamp(go * 255.0, 0.0, 255.0));
    out[off + 2] = static_cast<std::uint8_t>(std::clamp(bo * 255.0, 0.0, 255.0));
    out[off + 3] = static_cast<std::uint8_t>(std::clamp(ao * 255.0, 0.0, 255.0));
  }
}

void composite(const FloatPixmap& in1, const FloatPixmap& in2, FloatPixmap& dst, CompositeOp op,
               double k1, double k2, double k3, double k4) {
  const auto src1 = in1.data();
  const auto src2 = in2.data();
  auto out = dst.data();

  const std::size_t pixelCount = std::min({src1.size(), src2.size(), out.size()}) / 4;

  for (std::size_t i = 0; i < pixelCount; ++i) {
    const std::size_t off = i * 4;

    // Premultiplied RGBA values already in [0, 1].
    const double r1 = src1[off + 0];
    const double g1 = src1[off + 1];
    const double b1 = src1[off + 2];
    const double a1 = src1[off + 3];

    const double r2 = src2[off + 0];
    const double g2 = src2[off + 1];
    const double b2 = src2[off + 2];
    const double a2 = src2[off + 3];

    double ro = 0.0;
    double go = 0.0;
    double bo = 0.0;
    double ao = 0.0;

    auto porterDuff = [&](double fa, double fb) {
      ro = fa * r1 + fb * r2;
      go = fa * g1 + fb * g2;
      bo = fa * b1 + fb * b2;
      ao = fa * a1 + fb * a2;
    };

    switch (op) {
      case CompositeOp::Over: porterDuff(1.0, 1.0 - a1); break;
      case CompositeOp::In: porterDuff(a2, 0.0); break;
      case CompositeOp::Out: porterDuff(1.0 - a2, 0.0); break;
      case CompositeOp::Atop: porterDuff(a2, 1.0 - a1); break;
      case CompositeOp::Xor: porterDuff(1.0 - a2, 1.0 - a1); break;
      case CompositeOp::Lighter: porterDuff(1.0, 1.0); break;
      case CompositeOp::Arithmetic:
        ro = k1 * r1 * r2 + k2 * r1 + k3 * r2 + k4;
        go = k1 * g1 * g2 + k2 * g1 + k3 * g2 + k4;
        bo = k1 * b1 * b2 + k2 * b1 + k3 * b2 + k4;
        ao = k1 * a1 * a2 + k2 * a1 + k3 * a2 + k4;
        break;
    }

    out[off + 0] = static_cast<float>(std::clamp(ro, 0.0, 1.0));
    out[off + 1] = static_cast<float>(std::clamp(go, 0.0, 1.0));
    out[off + 2] = static_cast<float>(std::clamp(bo, 0.0, 1.0));
    out[off + 3] = static_cast<float>(std::clamp(ao, 0.0, 1.0));
  }
}

}  // namespace tiny_skia::filter
