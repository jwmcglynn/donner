#pragma once
/// @file
/// Shared turbulence storage layouts and precompiled shader projections.
#include <array>
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/programs/TurbulenceBindings.h"
namespace donner::gpu::shader::programs {
/// Noise admission and device-to-filter coordinate parameters.
struct TurbulenceParams {
  float baseFreqX;          //!< Base x frequency in filter space.
  float baseFreqY;          //!< Base y frequency in filter space.
  int32_t numOctaves;       //!< Admitted octave count.
  int32_t seed;             //!< Rounded deterministic noise seed.
  uint32_t stitchTiles;     //!< Nonzero wraps noise at the tile edges.
  uint32_t typeFlag;        //!< One for turbulence, zero for fractal noise.
  float tileWidth;          //!< Tile width in pixels.
  float tileHeight;         //!< Tile height in pixels.
  float filterFromDeviceA;  //!< Device-x coefficient of filter x.
  float filterFromDeviceB;  //!< Device-y coefficient of filter x.
  float filterFromDeviceC;  //!< Device-x coefficient of filter y.
  float filterFromDeviceD;  //!< Device-y coefficient of filter y.
};
/// Deterministic permutation and per-channel gradient tables.
struct TurbulenceTables {
  std::array<int32_t, kTurbulenceTableSize> lattice{};      //!< Permutation entries.
  std::array<float, kTurbulenceGradientTableSize> gradX{};  //!< Per-channel x gradients.
  std::array<float, kTurbulenceGradientTableSize> gradY{};  //!< Per-channel y gradients.
};
static_assert(sizeof(TurbulenceParams) == 48);
static_assert(sizeof(TurbulenceTables) == 18504);
/// Returns authored WGSL and reflection for bounded SVG turbulence or fractal noise.
/// The host supplies deterministic tables and admitted noise parameters; output is premultiplied.
/// @return Stable view into a process-lifetime artifact.
const CompiledShaderView& TurbulenceShader();
/// Returns only the platform-native MSL or SPIR-V projection and reflection.
/// @return Stable view into a process-lifetime artifact.
const CompiledShaderView& TurbulenceNativeShader();
}  // namespace donner::gpu::shader::programs
