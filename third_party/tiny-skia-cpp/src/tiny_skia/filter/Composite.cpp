#include "tiny_skia/filter/Composite.h"

#include <algorithm>
#include <cstddef>

#include "tiny_skia/filter/SimdVec.h"

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
  const float* s1 = src1.data();
  const float* s2 = src2.data();
  float* o = out.data();

  // Hoist switch outside loop + use Vec4f32 SIMD for all Porter-Duff operations.
  switch (op) {
    case CompositeOp::Over:
      for (std::size_t i = 0; i < pixelCount; ++i) {
        const std::size_t off = i * 4;
        Vec4f32 p1 = Vec4f32::load(s1 + off);
        Vec4f32 p2 = Vec4f32::load(s2 + off);
        // result = p1 + p2 * (1 - a1)
        Vec4f32::fmadd(p2, Vec4f32::splat(1.0f - s1[off + 3]), p1).clamp01().store(o + off);
      }
      break;
    case CompositeOp::In:
      for (std::size_t i = 0; i < pixelCount; ++i) {
        const std::size_t off = i * 4;
        (Vec4f32::load(s1 + off) * Vec4f32::splat(s2[off + 3])).clamp01().store(o + off);
      }
      break;
    case CompositeOp::Out:
      for (std::size_t i = 0; i < pixelCount; ++i) {
        const std::size_t off = i * 4;
        (Vec4f32::load(s1 + off) * Vec4f32::splat(1.0f - s2[off + 3])).clamp01().store(o + off);
      }
      break;
    case CompositeOp::Atop:
      for (std::size_t i = 0; i < pixelCount; ++i) {
        const std::size_t off = i * 4;
        Vec4f32 p1 = Vec4f32::load(s1 + off);
        Vec4f32 p2 = Vec4f32::load(s2 + off);
        Vec4f32::fmadd(p2, Vec4f32::splat(1.0f - s1[off + 3]),
                        p1 * Vec4f32::splat(s2[off + 3]))
            .clamp01()
            .store(o + off);
      }
      break;
    case CompositeOp::Xor:
      for (std::size_t i = 0; i < pixelCount; ++i) {
        const std::size_t off = i * 4;
        Vec4f32 p1 = Vec4f32::load(s1 + off);
        Vec4f32 p2 = Vec4f32::load(s2 + off);
        (p1 * Vec4f32::splat(1.0f - s2[off + 3]) + p2 * Vec4f32::splat(1.0f - s1[off + 3]))
            .clamp01()
            .store(o + off);
      }
      break;
    case CompositeOp::Lighter:
      for (std::size_t i = 0; i < pixelCount; ++i) {
        const std::size_t off = i * 4;
        (Vec4f32::load(s1 + off) + Vec4f32::load(s2 + off)).clamp01().store(o + off);
      }
      break;
    case CompositeOp::Arithmetic: {
      const Vec4f32 vk1 = Vec4f32::splat(static_cast<float>(k1));
      const Vec4f32 vk2 = Vec4f32::splat(static_cast<float>(k2));
      const Vec4f32 vk3 = Vec4f32::splat(static_cast<float>(k3));
      const Vec4f32 vk4 = Vec4f32::splat(static_cast<float>(k4));
      for (std::size_t i = 0; i < pixelCount; ++i) {
        const std::size_t off = i * 4;
        Vec4f32 p1 = Vec4f32::load(s1 + off);
        Vec4f32 p2 = Vec4f32::load(s2 + off);
        // k1*p1*p2 + k2*p1 + k3*p2 + k4
        Vec4f32 result = vk1 * p1 * p2 + vk2 * p1 + vk3 * p2 + vk4;
        result.clamp01().store(o + off);
      }
      break;
    }
  }
}

}  // namespace tiny_skia::filter
