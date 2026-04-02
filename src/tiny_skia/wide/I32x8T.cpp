#include "tiny_skia/wide/I32x8T.h"

#include <bit>

#include "tiny_skia/wide/F32x4T.h"
#include "tiny_skia/wide/F32x8T.h"
#include "tiny_skia/wide/I32x4T.h"
#include "tiny_skia/wide/U32x4T.h"
#include "tiny_skia/wide/U32x8T.h"
#include "tiny_skia/wide/backend/ScalarI32x8T.h"
#include "tiny_skia/wide/backend/X86Avx2FmaI32x8T.h"

namespace tiny_skia::wide {

namespace {

[[nodiscard]] constexpr bool useX86Avx2FmaI32x8() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__AVX2__) && defined(__FMA__) && \
    (defined(__x86_64__) || defined(__i386__))
  return true;
#else
  return false;
#endif
}

[[nodiscard]] constexpr bool useAarch64NeonI32x8() {
#if defined(TINYSKIA_CFG_IF_SIMD_NATIVE) && defined(__aarch64__) && defined(__ARM_NEON)
  return true;
#else
  return false;
#endif
}

[[maybe_unused]] [[nodiscard]] I32x4T lowI32x4(const std::array<std::int32_t, 8>& lanes) {
  return I32x4T({lanes[0], lanes[1], lanes[2], lanes[3]});
}

[[maybe_unused]] [[nodiscard]] I32x4T highI32x4(const std::array<std::int32_t, 8>& lanes) {
  return I32x4T({lanes[4], lanes[5], lanes[6], lanes[7]});
}

[[maybe_unused]] [[nodiscard]] std::array<std::int32_t, 8> joinI32Lanes(
    const std::array<std::int32_t, 4>& lo, const std::array<std::int32_t, 4>& hi) {
  return {lo[0], lo[1], lo[2], lo[3], hi[0], hi[1], hi[2], hi[3]};
}

[[maybe_unused]] [[nodiscard]] std::array<std::uint32_t, 4> bitcastI32ToU32(
    const std::array<std::int32_t, 4>& i) {
  return {
      std::bit_cast<std::uint32_t>(i[0]),
      std::bit_cast<std::uint32_t>(i[1]),
      std::bit_cast<std::uint32_t>(i[2]),
      std::bit_cast<std::uint32_t>(i[3]),
  };
}

[[maybe_unused]] [[nodiscard]] I32x8T composeI32x8(const I32x4T& lo, const I32x4T& hi) {
  return I32x8T(joinI32Lanes(lo.lanes(), hi.lanes()));
}

[[maybe_unused]] [[nodiscard]] F32x8T composeF32x8(const F32x4T& lo, const F32x4T& hi) {
  const auto loLanes = lo.lanes();
  const auto hiLanes = hi.lanes();
  return F32x8T({loLanes[0], loLanes[1], loLanes[2], loLanes[3], hiLanes[0], hiLanes[1], hiLanes[2],
                 hiLanes[3]});
}

[[maybe_unused]] [[nodiscard]] U32x8T composeU32x8(const U32x4T& lo, const U32x4T& hi) {
  const auto loLanes = lo.lanes();
  const auto hiLanes = hi.lanes();
  return U32x8T({loLanes[0], loLanes[1], loLanes[2], loLanes[3], hiLanes[0], hiLanes[1], hiLanes[2],
                 hiLanes[3]});
}

}  // namespace

I32x8T I32x8T::blend(const I32x8T& t, const I32x8T& f) const {
  if constexpr (useX86Avx2FmaI32x8()) {
    return I32x8T(backend::x86_avx2_fma::i32x8Blend(lanes_, t.lanes_, f.lanes_));
  }
  if constexpr (useAarch64NeonI32x8()) {
    return composeI32x8(lowI32x4(lanes_).blend(lowI32x4(t.lanes_), lowI32x4(f.lanes_)),
                        highI32x4(lanes_).blend(highI32x4(t.lanes_), highI32x4(f.lanes_)));
  }

  return I32x8T(backend::scalar::i32x8Blend(lanes_, t.lanes_, f.lanes_));
}

I32x8T I32x8T::cmpEq(const I32x8T& rhs) const {
  if constexpr (useX86Avx2FmaI32x8()) {
    return I32x8T(backend::x86_avx2_fma::i32x8CmpEq(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonI32x8()) {
    return composeI32x8(lowI32x4(lanes_).cmpEq(lowI32x4(rhs.lanes_)),
                        highI32x4(lanes_).cmpEq(highI32x4(rhs.lanes_)));
  }

  return I32x8T(backend::scalar::i32x8CmpEq(lanes_, rhs.lanes_));
}

I32x8T I32x8T::cmpGt(const I32x8T& rhs) const {
  if constexpr (useX86Avx2FmaI32x8()) {
    return I32x8T(backend::x86_avx2_fma::i32x8CmpGt(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonI32x8()) {
    return composeI32x8(lowI32x4(lanes_).cmpGt(lowI32x4(rhs.lanes_)),
                        highI32x4(lanes_).cmpGt(highI32x4(rhs.lanes_)));
  }

  return I32x8T(backend::scalar::i32x8CmpGt(lanes_, rhs.lanes_));
}

I32x8T I32x8T::cmpLt(const I32x8T& rhs) const {
  if constexpr (useX86Avx2FmaI32x8()) {
    return I32x8T(backend::x86_avx2_fma::i32x8CmpLt(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonI32x8()) {
    return composeI32x8(lowI32x4(lanes_).cmpLt(lowI32x4(rhs.lanes_)),
                        highI32x4(lanes_).cmpLt(highI32x4(rhs.lanes_)));
  }

  return I32x8T(backend::scalar::i32x8CmpLt(lanes_, rhs.lanes_));
}

F32x8T I32x8T::toF32x8() const {
  if constexpr (useX86Avx2FmaI32x8()) {
    return F32x8T(backend::x86_avx2_fma::i32x8ToF32(lanes_));
  }
  if constexpr (useAarch64NeonI32x8()) {
    return composeF32x8(lowI32x4(lanes_).toF32x4(), highI32x4(lanes_).toF32x4());
  }

  return F32x8T(backend::scalar::i32x8ToF32(lanes_));
}

U32x8T I32x8T::toU32x8Bitcast() const {
  if constexpr (useX86Avx2FmaI32x8()) {
    return U32x8T(backend::x86_avx2_fma::i32x8ToU32Bitcast(lanes_));
  }
  if constexpr (useAarch64NeonI32x8()) {
    return composeU32x8(U32x4T(bitcastI32ToU32(lowI32x4(lanes_).lanes())),
                        U32x4T(bitcastI32ToU32(highI32x4(lanes_).lanes())));
  }

  return U32x8T(backend::scalar::i32x8ToU32Bitcast(lanes_));
}

F32x8T I32x8T::toF32x8Bitcast() const {
  if constexpr (useX86Avx2FmaI32x8()) {
    return F32x8T(backend::x86_avx2_fma::i32x8ToF32Bitcast(lanes_));
  }
  if constexpr (useAarch64NeonI32x8()) {
    return composeF32x8(lowI32x4(lanes_).toF32x4Bitcast(), highI32x4(lanes_).toF32x4Bitcast());
  }

  return F32x8T(backend::scalar::i32x8ToF32Bitcast(lanes_));
}

I32x8T I32x8T::operator+(const I32x8T& rhs) const {
  if constexpr (useX86Avx2FmaI32x8()) {
    return I32x8T(backend::x86_avx2_fma::i32x8Add(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonI32x8()) {
    return composeI32x8(lowI32x4(lanes_) + lowI32x4(rhs.lanes_),
                        highI32x4(lanes_) + highI32x4(rhs.lanes_));
  }

  return I32x8T(backend::scalar::i32x8Add(lanes_, rhs.lanes_));
}

I32x8T I32x8T::operator*(const I32x8T& rhs) const {
  if constexpr (useX86Avx2FmaI32x8()) {
    return I32x8T(backend::x86_avx2_fma::i32x8Mul(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonI32x8()) {
    return composeI32x8(lowI32x4(lanes_) * lowI32x4(rhs.lanes_),
                        highI32x4(lanes_) * highI32x4(rhs.lanes_));
  }

  return I32x8T(backend::scalar::i32x8Mul(lanes_, rhs.lanes_));
}

I32x8T I32x8T::operator&(const I32x8T& rhs) const {
  if constexpr (useX86Avx2FmaI32x8()) {
    return I32x8T(backend::x86_avx2_fma::i32x8And(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonI32x8()) {
    return composeI32x8(lowI32x4(lanes_) & lowI32x4(rhs.lanes_),
                        highI32x4(lanes_) & highI32x4(rhs.lanes_));
  }

  return I32x8T(backend::scalar::i32x8And(lanes_, rhs.lanes_));
}

I32x8T I32x8T::operator|(const I32x8T& rhs) const {
  if constexpr (useX86Avx2FmaI32x8()) {
    return I32x8T(backend::x86_avx2_fma::i32x8Or(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonI32x8()) {
    return composeI32x8(lowI32x4(lanes_) | lowI32x4(rhs.lanes_),
                        highI32x4(lanes_) | highI32x4(rhs.lanes_));
  }

  return I32x8T(backend::scalar::i32x8Or(lanes_, rhs.lanes_));
}

I32x8T I32x8T::operator^(const I32x8T& rhs) const {
  if constexpr (useX86Avx2FmaI32x8()) {
    return I32x8T(backend::x86_avx2_fma::i32x8Xor(lanes_, rhs.lanes_));
  }
  if constexpr (useAarch64NeonI32x8()) {
    return composeI32x8(lowI32x4(lanes_) ^ lowI32x4(rhs.lanes_),
                        highI32x4(lanes_) ^ highI32x4(rhs.lanes_));
  }

  return I32x8T(backend::scalar::i32x8Xor(lanes_, rhs.lanes_));
}

}  // namespace tiny_skia::wide
