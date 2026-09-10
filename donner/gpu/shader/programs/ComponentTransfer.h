#pragma once
/// @file
/// Typed component-transfer filter with bounded, packed channel tables.
#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/ComponentTransferBindings.h"
namespace donner::gpu::shader::programs {
/**
 * Evaluates identity, table, discrete, linear and gamma functions on straight-alpha channels,
 * then premultiplies the result. Hosts validate at most 1024 table entries per channel and pack
 * four 32-byte function records followed by the float tables (at least one trailing float).
 * Every word is f32; integer kind, count and offset fields are represented exactly.
 * Source and output extents match. Channel-table offsets and counts refer to that packed block.
 */
ShaderResult<IrModule> BuildComponentTransferModule();
}  // namespace donner::gpu::shader::programs
