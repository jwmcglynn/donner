#include "tiny_skia/wide/U32x8T.h"

#include <bit>

#include "tiny_skia/wide/F32x8T.h"
#include "tiny_skia/wide/I32x8T.h"
#include "tiny_skia/wide/U32x4T.h"
#include "tiny_skia/wide/backend/ScalarU32x8T.h"
#include "tiny_skia/wide/backend/X86Avx2FmaU32x8T.h"

namespace tiny_skia::wide {

namespace {

[[nodiscard]] constexpr bool useAarch64NeonU32x8() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  return true;
#else
  return false;
#endif
}

[[nodiscard]] constexpr bool useX86Avx2FmaU32x8() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__AVX2__) && defined(__FMA__) && \
    (defined(__x86_64__) || defined(__i386__))
  return true;
#else
  return false;
#endif
}

[[maybe_unused]] [[nodiscard]] U32x4T lowU32x4(const std::array<std::uint32_t, 8>& lanes) {
  return U32x4T({lanes[0], lanes[1], lanes[2], lanes[3]});
}

[[maybe_unused]] [[nodiscard]] U32x4T highU32x4(const std::array<std::uint32_t, 8>& lanes) {
  return U32x4T({lanes[4], lanes[5], lanes[6], lanes[7]});
}

[[maybe_unused]] [[nodiscard]] U32x8T composeU32x8(const U32x4T& lo, const U32x4T& hi) {
  const auto loLanes = lo.lanes();
  const auto hiLanes = hi.lanes();
  return U32x8T({loLanes[0], loLanes[1], loLanes[2], loLanes[3], hiLanes[0], hiLanes[1], hiLanes[2],
                 hiLanes[3]});
}

[[maybe_unused]] [[nodiscard]] std::array<std::int32_t, 4> bitcastU32ToI32(
    const std::array<std::uint32_t, 4>& u) {
  return {
      std::bit_cast<std::int32_t>(u[0]),
      std::bit_cast<std::int32_t>(u[1]),
      std::bit_cast<std::int32_t>(u[2]),
      std::bit_cast<std::int32_t>(u[3]),
  };
}

}  // namespace

I32x8T U32x8T::toI32x8Bitcast() const {
  if constexpr (useX86Avx2FmaU32x8()) {
    return I32x8T(backend::x86_avx2_fma::u32x8ToI32Bitcast(lanes_));
  }
  if constexpr (useAarch64NeonU32x8()) {
    const auto lo = lowU32x4(lanes_).lanes();
    const auto hi = highU32x4(lanes_).lanes();
    const auto loI = bitcastU32ToI32(lo);
    const auto hiI = bitcastU32ToI32(hi);
    return I32x8T({loI[0], loI[1], loI[2], loI[3], hiI[0], hiI[1], hiI[2], hiI[3]});
  }

  return I32x8T(backend::scalar::u32x8ToI32Bitcast(lanes_));
}

F32x8T U32x8T::toF32x8Bitcast() const {
  if constexpr (useX86Avx2FmaU32x8()) {
    return F32x8T(backend::x86_avx2_fma::u32x8ToF32Bitcast(lanes_));
  }
  if constexpr (useAarch64NeonU32x8()) {
    return toI32x8Bitcast().toF32x8Bitcast();
  }

  return F32x8T(backend::scalar::u32x8ToF32Bitcast(lanes_));
}

U32x8T U32x8T::cmpEq(const U32x8T& rhs) const {
  if constexpr (useX86Avx2FmaU32x8()) {
    return U32x8T(backend::x86_avx2_fma::u32x8CmpEq(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonU32x8()) {
    return composeU32x8(lowU32x4(lanes_).cmpEq(lowU32x4(rhs.lanes_)),
                        highU32x4(lanes_).cmpEq(highU32x4(rhs.lanes_)));
  }

  return U32x8T(backend::scalar::u32x8CmpEq(lanes_, rhs.lanes_));
}

U32x8T U32x8T::cmpNe(const U32x8T& rhs) const {
  if constexpr (useX86Avx2FmaU32x8()) {
    return U32x8T(backend::x86_avx2_fma::u32x8CmpNe(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonU32x8()) {
    return composeU32x8(lowU32x4(lanes_).cmpNe(lowU32x4(rhs.lanes_)),
                        highU32x4(lanes_).cmpNe(highU32x4(rhs.lanes_)));
  }

  return U32x8T(backend::scalar::u32x8CmpNe(lanes_, rhs.lanes_));
}

U32x8T U32x8T::cmpLt(const U32x8T& rhs) const {
  if constexpr (useX86Avx2FmaU32x8()) {
    return U32x8T(backend::x86_avx2_fma::u32x8CmpLt(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonU32x8()) {
    return composeU32x8(lowU32x4(lanes_).cmpLt(lowU32x4(rhs.lanes_)),
                        highU32x4(lanes_).cmpLt(highU32x4(rhs.lanes_)));
  }

  return U32x8T(backend::scalar::u32x8CmpLt(lanes_, rhs.lanes_));
}

U32x8T U32x8T::cmpLe(const U32x8T& rhs) const {
  if constexpr (useX86Avx2FmaU32x8()) {
    return U32x8T(backend::x86_avx2_fma::u32x8CmpLe(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonU32x8()) {
    return composeU32x8(lowU32x4(lanes_).cmpLe(lowU32x4(rhs.lanes_)),
                        highU32x4(lanes_).cmpLe(highU32x4(rhs.lanes_)));
  }

  return U32x8T(backend::scalar::u32x8CmpLe(lanes_, rhs.lanes_));
}

U32x8T U32x8T::cmpGt(const U32x8T& rhs) const {
  if constexpr (useX86Avx2FmaU32x8()) {
    return U32x8T(backend::x86_avx2_fma::u32x8CmpGt(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonU32x8()) {
    return composeU32x8(lowU32x4(lanes_).cmpGt(lowU32x4(rhs.lanes_)),
                        highU32x4(lanes_).cmpGt(highU32x4(rhs.lanes_)));
  }

  return U32x8T(backend::scalar::u32x8CmpGt(lanes_, rhs.lanes_));
}

U32x8T U32x8T::cmpGe(const U32x8T& rhs) const {
  if constexpr (useX86Avx2FmaU32x8()) {
    return U32x8T(backend::x86_avx2_fma::u32x8CmpGe(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonU32x8()) {
    return composeU32x8(lowU32x4(lanes_).cmpGe(lowU32x4(rhs.lanes_)),
                        highU32x4(lanes_).cmpGe(highU32x4(rhs.lanes_)));
  }

  return U32x8T(backend::scalar::u32x8CmpGe(lanes_, rhs.lanes_));
}

U32x8T U32x8T::operator~() const {
  if constexpr (useX86Avx2FmaU32x8()) {
    return U32x8T(backend::x86_avx2_fma::u32x8Not(lanes_));
  }
  if constexpr (useAarch64NeonU32x8()) {
    return composeU32x8(~lowU32x4(lanes_), ~highU32x4(lanes_));
  }

  return U32x8T(backend::scalar::u32x8Not(lanes_));
}

U32x8T U32x8T::operator+(const U32x8T& rhs) const {
  if constexpr (useX86Avx2FmaU32x8()) {
    return U32x8T(backend::x86_avx2_fma::u32x8Add(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonU32x8()) {
    return composeU32x8(lowU32x4(lanes_) + lowU32x4(rhs.lanes_),
                        highU32x4(lanes_) + highU32x4(rhs.lanes_));
  }

  return U32x8T(backend::scalar::u32x8Add(lanes_, rhs.lanes_));
}

U32x8T U32x8T::operator&(const U32x8T& rhs) const {
  if constexpr (useX86Avx2FmaU32x8()) {
    return U32x8T(backend::x86_avx2_fma::u32x8And(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonU32x8()) {
    return composeU32x8(lowU32x4(lanes_) & lowU32x4(rhs.lanes_),
                        highU32x4(lanes_) & highU32x4(rhs.lanes_));
  }

  return U32x8T(backend::scalar::u32x8And(lanes_, rhs.lanes_));
}

U32x8T U32x8T::operator|(const U32x8T& rhs) const {
  if constexpr (useX86Avx2FmaU32x8()) {
    return U32x8T(backend::x86_avx2_fma::u32x8Or(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonU32x8()) {
    return composeU32x8(lowU32x4(lanes_) | lowU32x4(rhs.lanes_),
                        highU32x4(lanes_) | highU32x4(rhs.lanes_));
  }

  return U32x8T(backend::scalar::u32x8Or(lanes_, rhs.lanes_));
}

U32x8T U32x8T::operator^(const U32x8T& rhs) const {
  if constexpr (useX86Avx2FmaU32x8()) {
    return U32x8T(backend::x86_avx2_fma::u32x8Xor(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonU32x8()) {
    return composeU32x8(lowU32x4(lanes_) ^ lowU32x4(rhs.lanes_),
                        highU32x4(lanes_) ^ highU32x4(rhs.lanes_));
  }

  return U32x8T(backend::scalar::u32x8Xor(lanes_, rhs.lanes_));
}

}  // namespace tiny_skia::wide
