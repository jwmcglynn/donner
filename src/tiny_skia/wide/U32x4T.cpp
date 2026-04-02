#include "tiny_skia/wide/U32x4T.h"

#include <array>
#include <cstdint>

#include "tiny_skia/wide/backend/Aarch64NeonU32x4T.h"
#include "tiny_skia/wide/backend/ScalarU32x4T.h"
#include "tiny_skia/wide/backend/X86Avx2FmaU32x8T.h"

namespace tiny_skia::wide {

namespace {

[[nodiscard]] constexpr bool useAarch64NeonU32x4() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  return true;
#else
  return false;
#endif
}

[[nodiscard]] constexpr bool useX86Avx2FmaU32x4() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__AVX2__) && defined(__FMA__) && \
    (defined(__x86_64__) || defined(__i386__))
  return true;
#else
  return false;
#endif
}

[[nodiscard]] [[maybe_unused]] std::array<std::uint32_t, 8> widenU32x4(const std::array<std::uint32_t, 4>& lanes) {
  return {lanes[0], lanes[1], lanes[2], lanes[3], 0u, 0u, 0u, 0u};
}

[[nodiscard]] [[maybe_unused]] std::array<std::uint32_t, 4> narrowU32x8(const std::array<std::uint32_t, 8>& lanes) {
  return {lanes[0], lanes[1], lanes[2], lanes[3]};
}

}  // namespace

U32x4T U32x4T::cmpEq(const U32x4T& rhs) const {
  if constexpr (useX86Avx2FmaU32x4()) {
    return U32x4T(
        narrowU32x8(backend::x86_avx2_fma::u32x8CmpEq(widenU32x4(lanes_), widenU32x4(rhs.lanes_))));
  }
  if constexpr (useAarch64NeonU32x4()) {
    return U32x4T(backend::aarch64_neon::u32x4CmpEq(lanes_, rhs.lanes_));
  }

  return U32x4T(backend::scalar::u32x4CmpEq(lanes_, rhs.lanes_));
}

U32x4T U32x4T::cmpNe(const U32x4T& rhs) const {
  if constexpr (useX86Avx2FmaU32x4()) {
    return U32x4T(
        narrowU32x8(backend::x86_avx2_fma::u32x8CmpNe(widenU32x4(lanes_), widenU32x4(rhs.lanes_))));
  }
  if constexpr (useAarch64NeonU32x4()) {
    return U32x4T(backend::aarch64_neon::u32x4CmpNe(lanes_, rhs.lanes_));
  }

  return U32x4T(backend::scalar::u32x4CmpNe(lanes_, rhs.lanes_));
}

U32x4T U32x4T::cmpLt(const U32x4T& rhs) const {
  if constexpr (useX86Avx2FmaU32x4()) {
    return U32x4T(
        narrowU32x8(backend::x86_avx2_fma::u32x8CmpLt(widenU32x4(lanes_), widenU32x4(rhs.lanes_))));
  }
  if constexpr (useAarch64NeonU32x4()) {
    return U32x4T(backend::aarch64_neon::u32x4CmpLt(lanes_, rhs.lanes_));
  }

  return U32x4T(backend::scalar::u32x4CmpLt(lanes_, rhs.lanes_));
}

U32x4T U32x4T::cmpLe(const U32x4T& rhs) const {
  if constexpr (useX86Avx2FmaU32x4()) {
    return U32x4T(
        narrowU32x8(backend::x86_avx2_fma::u32x8CmpLe(widenU32x4(lanes_), widenU32x4(rhs.lanes_))));
  }
  if constexpr (useAarch64NeonU32x4()) {
    return U32x4T(backend::aarch64_neon::u32x4CmpLe(lanes_, rhs.lanes_));
  }

  return U32x4T(backend::scalar::u32x4CmpLe(lanes_, rhs.lanes_));
}

U32x4T U32x4T::cmpGt(const U32x4T& rhs) const {
  if constexpr (useX86Avx2FmaU32x4()) {
    return U32x4T(
        narrowU32x8(backend::x86_avx2_fma::u32x8CmpGt(widenU32x4(lanes_), widenU32x4(rhs.lanes_))));
  }
  if constexpr (useAarch64NeonU32x4()) {
    return U32x4T(backend::aarch64_neon::u32x4CmpGt(lanes_, rhs.lanes_));
  }

  return U32x4T(backend::scalar::u32x4CmpGt(lanes_, rhs.lanes_));
}

U32x4T U32x4T::cmpGe(const U32x4T& rhs) const {
  if constexpr (useX86Avx2FmaU32x4()) {
    return U32x4T(
        narrowU32x8(backend::x86_avx2_fma::u32x8CmpGe(widenU32x4(lanes_), widenU32x4(rhs.lanes_))));
  }
  if constexpr (useAarch64NeonU32x4()) {
    return U32x4T(backend::aarch64_neon::u32x4CmpGe(lanes_, rhs.lanes_));
  }

  return U32x4T(backend::scalar::u32x4CmpGe(lanes_, rhs.lanes_));
}

U32x4T U32x4T::operator~() const {
  if constexpr (useX86Avx2FmaU32x4()) {
    return U32x4T(narrowU32x8(backend::x86_avx2_fma::u32x8Not(widenU32x4(lanes_))));
  }
  if constexpr (useAarch64NeonU32x4()) {
    return U32x4T(backend::aarch64_neon::u32x4Not(lanes_));
  }

  return U32x4T(backend::scalar::u32x4Not(lanes_));
}

U32x4T U32x4T::operator+(const U32x4T& rhs) const {
  if constexpr (useX86Avx2FmaU32x4()) {
    return U32x4T(
        narrowU32x8(backend::x86_avx2_fma::u32x8Add(widenU32x4(lanes_), widenU32x4(rhs.lanes_))));
  }
  if constexpr (useAarch64NeonU32x4()) {
    return U32x4T(backend::aarch64_neon::u32x4Add(lanes_, rhs.lanes_));
  }

  return U32x4T(backend::scalar::u32x4Add(lanes_, rhs.lanes_));
}

U32x4T U32x4T::operator&(const U32x4T& rhs) const {
  if constexpr (useX86Avx2FmaU32x4()) {
    return U32x4T(
        narrowU32x8(backend::x86_avx2_fma::u32x8And(widenU32x4(lanes_), widenU32x4(rhs.lanes_))));
  }
  if constexpr (useAarch64NeonU32x4()) {
    return U32x4T(backend::aarch64_neon::u32x4And(lanes_, rhs.lanes_));
  }

  return U32x4T(backend::scalar::u32x4And(lanes_, rhs.lanes_));
}

U32x4T U32x4T::operator|(const U32x4T& rhs) const {
  if constexpr (useX86Avx2FmaU32x4()) {
    return U32x4T(
        narrowU32x8(backend::x86_avx2_fma::u32x8Or(widenU32x4(lanes_), widenU32x4(rhs.lanes_))));
  }
  if constexpr (useAarch64NeonU32x4()) {
    return U32x4T(backend::aarch64_neon::u32x4Or(lanes_, rhs.lanes_));
  }

  return U32x4T(backend::scalar::u32x4Or(lanes_, rhs.lanes_));
}

U32x4T U32x4T::operator^(const U32x4T& rhs) const {
  if constexpr (useX86Avx2FmaU32x4()) {
    return U32x4T(
        narrowU32x8(backend::x86_avx2_fma::u32x8Xor(widenU32x4(lanes_), widenU32x4(rhs.lanes_))));
  }
  if constexpr (useAarch64NeonU32x4()) {
    return U32x4T(backend::aarch64_neon::u32x4Xor(lanes_, rhs.lanes_));
  }

  return U32x4T(backend::scalar::u32x4Xor(lanes_, rhs.lanes_));
}

}  // namespace tiny_skia::wide
