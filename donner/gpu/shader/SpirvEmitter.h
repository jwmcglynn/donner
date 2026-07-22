#pragma once
/// @file
/// Deterministic SPIR-V binary emission from a \c donner::gpu::shader IR module.

#include <cstdint>
#include <vector>

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/ShaderResult.h"

namespace donner::gpu::shader {

/**
 * Emits deterministic SPIR-V words for \p module (design 0053 "Original emitters").
 *
 * Targets SPIR-V 1.3 for Vulkan 1.1: Logical addressing, the GLSL450 memory model, the Shader
 * capability (plus ImageQuery only when the module calls textureDimensions), and the
 * GLSL.std.450 extended instruction set for math builtins. The result is the standard word
 * stream (magic 0x07230203 first); callers serialize it little-endian per word for `.spv` files.
 *
 * Determinism: identical input modules produce byte-identical output. IDs are assigned from a
 * single counter starting at 1 in a fixed traversal order: the GLSL.std.450 import first, then
 * resource bindings in declaration order, module constants in declaration order, and functions
 * in declaration order (for entry points: stage IO variables in parameter-then-output order,
 * then the function type, function id, and body ids in statement/operand order). Types and
 * constants are created on first use and deduplicated through ordered maps keyed by structural
 * strings; no pointer values or addresses influence the output.
 *
 * Mapping highlights:
 * - Uniform buffers are Block-decorated structs in the Uniform storage class; read-only storage
 *   buffers use the StorageBuffer storage class with a NonWritable variable decoration, and
 *   runtime-array roots are wrapped in a synthesized Block struct whose member 0 is the array.
 *   Offset / ArrayStride / ColMajor / MatrixStride 16 decorations come from the IrLayout engine
 *   (never recomputed), so a uniform array whose natural stride was padded to 16 is expressible
 *   directly via ArrayStride (unlike the WGSL/MSL emitters, no padded wrapper is needed).
 * - Buffer types carry explicit layout decorations, so laid-out struct/array types are distinct
 *   from the undecorated types used for locals. Loading a struct or array from a buffer rebuilds
 *   the plain value member-by-member through access chains (SPIR-V 1.3 has no OpCopyLogical).
 * - WGSL value semantics: `let` bindings are SSA values; `var` declarations become Function
 *   storage OpVariables, all declared in the entry block per the specification, with
 *   initializers lowered to OpStore at the declaration point. Assignments to member / index /
 *   single-component-swizzle chains lower to OpAccessChain + OpStore.
 * - Structured control flow: if/else uses OpSelectionMerge; for-loops use OpLoopMerge with
 *   header, condition, body, continue, and merge blocks; `break` branches to the merge block and
 *   `continue` to the continue block. `discard` lowers to OpKill; statements after a terminator
 *   in the same IR block are unreachable and dropped.
 * - Builtins: abs/min/max/clamp pick FAbs/SAbs, FMin/SMin/UMin, FMax/SMax/UMax, and
 *   FClamp/SClamp/UClamp by scalar kind (abs of u32 is the identity); saturate is
 *   FClamp(x, 0, 1); round uses GLSL.std.450 RoundEven because WGSL `round` mandates
 *   round-half-to-even (plain Round leaves halfway cases undefined); select(f, t, c) maps to
 *   OpSelect(c, t, f); fwidth maps to OpFwidth; textureSample to OpSampledImage +
 *   OpImageSampleImplicitLod; textureLoad to OpImageFetch with a Lod operand; and
 *   textureDimensions to OpImageQuerySizeLod at level 0 (required for sampled images, which is
 *   why it carries the ImageQuery capability).
 * - IR expressions are side-effect free (all bindings are read-only), so `&&` / `||` lower to
 *   the eager OpLogicalAnd / OpLogicalOr.
 * - Entry point IO: inputs/outputs become Input/Output variables with Location decorations;
 *   the vertex position output is BuiltIn Position, the fragment position input is BuiltIn
 *   FragCoord, and the vertex instance-index input is BuiltIn InstanceIndex. Integer-typed
 *   fragment inputs receive the Flat decoration as WGSL semantics require. Fragment entry
 *   points declare OriginUpperLeft.
 *
 * Fails closed with a \ref ShaderError (never emits invalid SPIR-V silently) when:
 * - a float literal is non-finite;
 * - a binding type has no host-shareable layout (the IrLayout engine's bool/resource rejection
 *   is propagated);
 * - a plain function declares a texture or sampler parameter (module-scope bindings are already
 *   visible everywhere; parameter passing of opaque handles is out of scope);
 * - a runtime-sized array is loaded as a whole value, or an array rvalue (not backed by a
 *   pointer) is indexed dynamically;
 * - fwidth / textureSample / discard appear in a vertex entry point (defense in depth; the IR
 *   builder already rejects these).
 *
 * @param module Module to emit.
 */
ShaderResult<std::vector<uint32_t>> EmitSpirv(const IrModule& module);

}  // namespace donner::gpu::shader
