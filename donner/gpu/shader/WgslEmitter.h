#pragma once
/// @file
/// Deterministic WGSL text emission from a \c donner::gpu::shader IR module.

#include <string>

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/ShaderResult.h"

namespace donner::gpu::shader {

/**
 * Emits deterministic WGSL text for \p module (design 0053 "Original emitters").
 *
 * Output properties:
 * - Declarations follow module declaration order; struct definitions are emitted first, in
 *   dependency order (member structs before the structs that contain them).
 * - Stable formatting: two-space indentation, one statement per line, LF line endings, no
 *   trailing whitespace, and shortest-round-trip float formatting (the same std::format style
 *   the IR serializer uses) with explicit `f`/`i`/`u` literal suffixes.
 * - Every binary and unary subexpression is parenthesized, so emitted precedence never depends
 *   on WGSL precedence rules.
 * - Entry point IO: parameters are emitted inline with `@location`/`@builtin` annotations; each
 *   entry point returns a generated `<name>_Output` struct with annotated members.
 *
 * Fails closed with a \ref ShaderError (never emits invalid WGSL silently) when:
 * - an identifier collides with a WGSL reserved word;
 * - a uniform binding contains an array whose natural stride required 16-byte padding
 *   (`ArrayStrideInfo::paddedFromNatural`); materializing padded element wrappers is a
 *   documented emitter obligation this packet does not need (the solid-fill
 *   `clipPolygonPlanes: array<vec4<f32>, 4>` has a natural stride of 16), so the case is
 *   rejected instead of silently emitting a layout mismatch;
 * - a float literal is non-finite;
 * - two distinct struct types share a name.
 *
 * @param module Module to emit.
 */
ShaderResult<std::string> EmitWgsl(const IrModule& module);

}  // namespace donner::gpu::shader
