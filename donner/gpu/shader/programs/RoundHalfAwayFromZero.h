#pragma once
/// @file
/// The round-half-away-from-zero recipe, as a function any IR program can declare.
///
/// WGSL's `round` is round-half-to-even, so it disagrees with the CPU filter path's `std::round`
/// on every exact half. `sign(x) * floor(abs(x) + 0.5)` is the composition that agrees, and it is
/// declared here once so a program that needs it calls the same three opcodes in the same order
/// rather than re-deriving the rule.

#include <string_view>

#include "donner/gpu/shader/IrModule.h"

namespace donner::gpu::shader::programs {

/// Name the recipe is declared under, spelled the same by every module that declares it.
inline constexpr std::string_view kRoundHalfAwayFromZeroName = "round_half_away_from_zero";

/**
 * Declares `round_half_away_from_zero(x: f32) -> f32` on \p builder, ready to be called by name.
 *
 * Declare it before the function that calls it: a module emits its functions in declaration
 * order, and a call to a function declared later would not compile.
 *
 * @param builder Module to declare the function on.
 */
ShaderStatus AddRoundHalfAwayFromZero(ModuleBuilder& builder);

}  // namespace donner::gpu::shader::programs
