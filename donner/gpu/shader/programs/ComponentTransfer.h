#pragma once
/// @file
/// feComponentTransfer packed-record layout and precompiled shader projections.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Words in one channel record: kind, table offset, table count, slope, intercept,
/// amplitude, exponent, offset.
inline constexpr uint32_t kComponentTransferChannelWords = 8;
/// Words before the concatenated table values: four channel records.
inline constexpr uint32_t kComponentTransferHeaderWords = 4 * kComponentTransferChannelWords;
/// Returns the WGSL component-transfer artifact: four packed channel functions over a read-only
/// float array.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& ComponentTransferShader();
/// Returns only the platform-native ComponentTransfer projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& ComponentTransferNativeShader();
}  // namespace donner::gpu::shader::programs
