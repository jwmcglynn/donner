#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "tiny_skia/wide/Wide.h"

namespace tiny_skia::wide {

class U16x16T {
 public:
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  U16x16T() : lo_(vdupq_n_u16(0)), hi_(vdupq_n_u16(0)) {}

  explicit U16x16T(std::array<std::uint16_t, 16> lanes) {
    const auto pair = vld1q_u16_x2(lanes.data());
    lo_ = pair.val[0];
    hi_ = pair.val[1];
  }

  U16x16T(uint16x8_t lo, uint16x8_t hi) : lo_(lo), hi_(hi) {}

  [[nodiscard]] static U16x16T splat(std::uint16_t n) {
    return U16x16T(vdupq_n_u16(n), vdupq_n_u16(n));
  }

  /// Materializes lanes array by value (stores NEON regs to memory).
  [[nodiscard]] std::array<std::uint16_t, 16> lanes() const {
    std::array<std::uint16_t, 16> out{};
    const uint16x8x2_t pair = {{lo_, hi_}};
    vst1q_u16_x2(out.data(), pair);
    return out;
  }

  [[nodiscard]] uint16x8_t neonLo() const { return lo_; }
  [[nodiscard]] uint16x8_t neonHi() const { return hi_; }

#else
  U16x16T() = default;
  explicit constexpr U16x16T(std::array<std::uint16_t, 16> lanes) : lanes_(lanes) {}

  [[nodiscard]] static constexpr U16x16T splat(std::uint16_t n) {
    return U16x16T({n, n, n, n, n, n, n, n, n, n, n, n, n, n, n, n});
  }

  [[nodiscard]] constexpr const std::array<std::uint16_t, 16>& lanes() const { return lanes_; }

  [[nodiscard]] constexpr std::array<std::uint16_t, 16>& lanes() { return lanes_; }
#endif

  [[nodiscard]] U16x16T min(const U16x16T& rhs) const;
  [[nodiscard]] U16x16T max(const U16x16T& rhs) const;
  [[nodiscard]] U16x16T cmpLe(const U16x16T& rhs) const;
  [[nodiscard]] U16x16T blend(const U16x16T& t, const U16x16T& e) const;

  [[nodiscard]] U16x16T operator+(const U16x16T& rhs) const;
  [[nodiscard]] U16x16T operator-(const U16x16T& rhs) const;
  [[nodiscard]] U16x16T operator*(const U16x16T& rhs) const;
  [[nodiscard]] U16x16T operator/(const U16x16T& rhs) const;
  [[nodiscard]] U16x16T operator&(const U16x16T& rhs) const;
  [[nodiscard]] U16x16T operator|(const U16x16T& rhs) const;
  [[nodiscard]] U16x16T operator~() const;
  [[nodiscard]] U16x16T operator>>(const U16x16T& rhs) const;

 private:
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  uint16x8_t lo_;
  uint16x8_t hi_;
#else
  std::array<std::uint16_t, 16> lanes_{};
#endif
};

}  // namespace tiny_skia::wide

#include "tiny_skia/wide/backend/Aarch64NeonU16x16T.h"
#include "tiny_skia/wide/backend/ScalarU16x16T.h"
#include "tiny_skia/wide/backend/X86Avx2FmaU16x16T.h"

namespace tiny_skia::wide {

namespace {

[[nodiscard]] constexpr bool useX86Avx2FmaU16x16() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__AVX2__) && defined(__FMA__) && \
    (defined(__x86_64__) || defined(__i386__))
  return true;
#else
  return false;
#endif
}

[[nodiscard]] constexpr bool useAarch64NeonU16x16() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  return true;
#else
  return false;
#endif
}

}  // namespace

inline U16x16T U16x16T::min(const U16x16T& rhs) const {
  if constexpr (useX86Avx2FmaU16x16()) {
    return backend::x86_avx2_fma::u16x16Min(*this, rhs);
  }
  if constexpr (useAarch64NeonU16x16()) {
    return backend::aarch64_neon::u16x16Min(*this, rhs);
  }

  return backend::scalar::u16x16Min(*this, rhs);
}

inline U16x16T U16x16T::max(const U16x16T& rhs) const {
  if constexpr (useX86Avx2FmaU16x16()) {
    return backend::x86_avx2_fma::u16x16Max(*this, rhs);
  }
  if constexpr (useAarch64NeonU16x16()) {
    return backend::aarch64_neon::u16x16Max(*this, rhs);
  }

  return backend::scalar::u16x16Max(*this, rhs);
}

inline U16x16T U16x16T::cmpLe(const U16x16T& rhs) const {
  if constexpr (useX86Avx2FmaU16x16()) {
    return backend::x86_avx2_fma::u16x16CmpLe(*this, rhs);
  }
  if constexpr (useAarch64NeonU16x16()) {
    return backend::aarch64_neon::u16x16CmpLe(*this, rhs);
  }

  return backend::scalar::u16x16CmpLe(*this, rhs);
}

inline U16x16T U16x16T::blend(const U16x16T& t, const U16x16T& e) const {
  if constexpr (useX86Avx2FmaU16x16()) {
    return backend::x86_avx2_fma::u16x16Blend(*this, t, e);
  }
  if constexpr (useAarch64NeonU16x16()) {
    return backend::aarch64_neon::u16x16Blend(*this, t, e);
  }

  return backend::scalar::u16x16Blend(*this, t, e);
}

inline U16x16T U16x16T::operator+(const U16x16T& rhs) const {
  if constexpr (useX86Avx2FmaU16x16()) {
    return backend::x86_avx2_fma::u16x16Add(*this, rhs);
  }
  if constexpr (useAarch64NeonU16x16()) {
    return backend::aarch64_neon::u16x16Add(*this, rhs);
  }

  return backend::scalar::u16x16Add(*this, rhs);
}

inline U16x16T U16x16T::operator-(const U16x16T& rhs) const {
  if constexpr (useX86Avx2FmaU16x16()) {
    return backend::x86_avx2_fma::u16x16Sub(*this, rhs);
  }
  if constexpr (useAarch64NeonU16x16()) {
    return backend::aarch64_neon::u16x16Sub(*this, rhs);
  }

  return backend::scalar::u16x16Sub(*this, rhs);
}

inline U16x16T U16x16T::operator*(const U16x16T& rhs) const {
  if constexpr (useX86Avx2FmaU16x16()) {
    return backend::x86_avx2_fma::u16x16Mul(*this, rhs);
  }
  if constexpr (useAarch64NeonU16x16()) {
    return backend::aarch64_neon::u16x16Mul(*this, rhs);
  }

  return backend::scalar::u16x16Mul(*this, rhs);
}

inline U16x16T U16x16T::operator/(const U16x16T& rhs) const {
  return backend::scalar::u16x16Div(*this, rhs);
}

inline U16x16T U16x16T::operator&(const U16x16T& rhs) const {
  if constexpr (useX86Avx2FmaU16x16()) {
    return backend::x86_avx2_fma::u16x16And(*this, rhs);
  }
  if constexpr (useAarch64NeonU16x16()) {
    return backend::aarch64_neon::u16x16And(*this, rhs);
  }

  return backend::scalar::u16x16And(*this, rhs);
}

inline U16x16T U16x16T::operator|(const U16x16T& rhs) const {
  if constexpr (useX86Avx2FmaU16x16()) {
    return backend::x86_avx2_fma::u16x16Or(*this, rhs);
  }
  if constexpr (useAarch64NeonU16x16()) {
    return backend::aarch64_neon::u16x16Or(*this, rhs);
  }

  return backend::scalar::u16x16Or(*this, rhs);
}

inline U16x16T U16x16T::operator~() const {
  if constexpr (useX86Avx2FmaU16x16()) {
    return backend::x86_avx2_fma::u16x16Not(*this);
  }
  if constexpr (useAarch64NeonU16x16()) {
    return backend::aarch64_neon::u16x16Not(*this);
  }

  return backend::scalar::u16x16Not(*this);
}

inline U16x16T U16x16T::operator>>(const U16x16T& rhs) const {
  if constexpr (useX86Avx2FmaU16x16()) {
    return backend::x86_avx2_fma::u16x16Shr(*this, rhs);
  }

  return backend::scalar::u16x16Shr(*this, rhs);
}

}  // namespace tiny_skia::wide
