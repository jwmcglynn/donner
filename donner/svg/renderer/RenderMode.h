#pragma once
/**
 * @file RenderMode.h
 *
 * Defines the \ref donner::svg::RenderMode enum, which configures rendering behavior for
 * asynchronous resources like fonts.
 */

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * Rendering policy for asynchronous resource loads such as fonts.
 *
 * This controls whether rendering blocks until resources are loaded or proceeds with fallbacks.
 */
enum class RenderMode : uint8_t {
  /**
   * [DEFAULT] One-shot rendering: Block until all resources are loaded before rendering.
   *
   * This ensures the first render has all resources available, at the cost of initial delay.
   */
  OneShot,

  /**
   * Continuous rendering: Render immediately with fallbacks, re-render as resources load.
   *
   * This provides faster initial rendering but may show content reflow as resources arrive.
   */
  Continuous,
};

/**
 * Ostream output operator for \ref RenderMode enum.
 */
inline std::ostream& operator<<(std::ostream& os, RenderMode value) {
  switch (value) {
    case RenderMode::OneShot: return os << "one-shot";
    case RenderMode::Continuous: return os << "continuous";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
