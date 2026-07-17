#pragma once
/// @file
/// Deterministic Metal Shading Language emission from a \c donner::gpu::shader IR module.

#include <string>

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/ShaderResult.h"

namespace donner::gpu::shader {

/**
 * Emits deterministic MSL text for \p module (design 0053 "Original emitters").
 *
 * Follows the same determinism discipline as \ref EmitWgsl: declaration-order emission with
 * dependency-ordered struct definitions, two-space indentation, LF-only lines with no trailing
 * whitespace, shortest-round-trip float formatting (C++-legal: a `.0` is added when the shortest
 * form has no decimal point or exponent, then `f`), and fully parenthesized subexpressions.
 *
 * Mapping highlights:
 * - Types: f32 -> float, vecN<T> -> floatN/intN/uintN/boolN, mat4x4f -> float4x4, sized arrays
 *   -> C arrays, structs -> C++ structs. The MSL natural layout of every buffer-referenced
 *   struct is verified member-by-member against the WGSL layout engine
 *   (\ref ComputeStructLayout); any divergence (for example MSL's 16-byte float3, or a uniform
 *   array whose WGSL stride was padded to 16) fails closed instead of emitting a silently
 *   mismatched layout.
 * - Bindings use the argument-table map in MslBindingMap.h. MSL has no module-scope resources,
 *   so every module binding becomes a parameter: entry points receive them with
 *   `[[buffer]]`/`[[texture]]`/`[[sampler]]` attributes, plain functions receive them as leading
 *   parameters in module declaration order, and user-call sites forward them. Storage buffers
 *   are `device const` pointers (read-only expressed via const; `device` rather than `constant`
 *   address space so buffer sizes are unconstrained), uniform buffers are `constant T&`.
 * - Entry points: the vertex stage takes a generated `<name>_Input` struct via `[[stage_in]]`
 *   (fields carry `[[attribute(location)]]`) plus `[[instance_id]]`; the fragment stage takes a
 *   generated input struct whose members carry `[[position]]` / `[[user(locnN)]]`. Outputs are
 *   generated structs with `[[position]]` / `[[user(locnN)]]` (vertex) or `[[color(N)]]`
 *   (fragment). Parameter names are aliased locally so IR references emit unchanged.
 * - Builtins: saturate/fract/fwidth/abs/min/max/clamp/sqrt/length/round map to the metal
 *   namespace functions (via `using namespace metal;`), `select(f, t, c)` maps to
 *   `(c ? t : f)`, textureSample -> `tex.sample(smp, uv)`, textureLoad ->
 *   `tex.read(uint2(coords), uint(level))`, textureDimensions ->
 *   `uint2(tex.get_width(), tex.get_height())`, and `discard` -> `discard_fragment()`.
 *
 * @param module Module to emit.
 */
ShaderResult<std::string> EmitMsl(const IrModule& module);

}  // namespace donner::gpu::shader
