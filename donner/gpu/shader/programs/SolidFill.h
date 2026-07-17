#pragma once
/// @file
/// The complete solid-fill (Slug analytic dual-ray) pipeline program as shader IR.

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/ShaderResult.h"

namespace donner::gpu::shader::programs {

/**
 * Builds the complete solid-fill IR module: a faithful semantic re-expression of the Donner-owned
 * `donner/svg/renderer/geode/shaders/slug_fill.wgsl` (the analytic dual-ray coverage algorithm of
 * design docs 0041/0042).
 *
 * Contents mirror the WGSL shader function by function: the 288-byte `Uniforms` struct, `Band`
 * (32 bytes) and `InstanceTransform` (32 bytes) storage layouts, all 12 bindings at group 0 with
 * identical binding numbers, the `kNoBand` sentinel constant, the `vs_main` vertex stage
 * (per-instance matrix construction and dynamic half-pixel dilation), and the `fs_main` fragment
 * stage (dense band-grid lookup, `accumulateHoriz`/`accumulateVert` analytic ray coverage with
 * `owns_axis_sample` and the Citardauq-form `solve_quadratic`, `calc_coverage` blending,
 * non-zero and even-odd fill rules, convex clip-polygon planes, clip-mask coverage, and the
 * solid premultiplied color and repeat-tiled pattern paint paths).
 *
 * Entry points are named `vs_main` / `fs_main`; struct-typed WGSL stage IO is flattened to
 * annotated parameters/outputs with identical locations and builtins, and `.rgba` swizzles are
 * transliterated to `.xyzw`. The packet 6 Metal vertical slice compares pixels against a frozen
 * baseline rendered by the original WGSL shader, so behavior-affecting constructs must remain
 * semantically identical.
 */
ShaderResult<IrModule> BuildSolidFillModule();

}  // namespace donner::gpu::shader::programs
