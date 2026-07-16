#pragma once
/// @file
/// Overflow-checked integer arithmetic for GPU byte-size and extent math.

#include <cstdint>
#include <optional>

namespace donner::gpu {

/**
 * Overflow-checked addition of unsigned 64-bit sizes.
 *
 * All byte-size and extent arithmetic in the GPU runtime is checked (design
 * 0053 "Security and Reliability"): untrusted SVG content controls geometry
 * volume and image sizes, so silent wraparound must be impossible.
 *
 * @param a First operand.
 * @param b Second operand.
 * @return The sum, or \c std::nullopt on overflow.
 */
inline std::optional<uint64_t> CheckedAdd(uint64_t a, uint64_t b) {
  uint64_t result = 0;
  if (__builtin_add_overflow(a, b, &result)) {
    return std::nullopt;
  }
  return result;
}

/**
 * Overflow-checked multiplication of unsigned 64-bit sizes.
 *
 * @param a First operand.
 * @param b Second operand.
 * @return The product, or \c std::nullopt on overflow.
 */
inline std::optional<uint64_t> CheckedMul(uint64_t a, uint64_t b) {
  uint64_t result = 0;
  if (__builtin_mul_overflow(a, b, &result)) {
    return std::nullopt;
  }
  return result;
}

}  // namespace donner::gpu
