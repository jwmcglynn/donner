// Links both projections of every shader family a production Geode library links, so the
// projection inspector can confirm one linked binary carries the authored WGSL and the
// platform-native payload for each of them.

#include <cstddef>
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/programs/Checkerboard.h"
#include "donner/gpu/shader/programs/ColorSpaceConvert.h"
#include "donner/gpu/shader/programs/ComponentTransfer.h"
#include "donner/gpu/shader/programs/Composite.h"
#include "donner/gpu/shader/programs/ConvolveMatrix.h"
#include "donner/gpu/shader/programs/DiffuseLighting.h"
#include "donner/gpu/shader/programs/DisplacementMap.h"
#include "donner/gpu/shader/programs/DropShadow.h"
#include "donner/gpu/shader/programs/FilterBlend.h"
#include "donner/gpu/shader/programs/FilterColorMatrix.h"
#include "donner/gpu/shader/programs/FilterImage.h"
#include "donner/gpu/shader/programs/FilterResolve.h"
#include "donner/gpu/shader/programs/Flood.h"
#include "donner/gpu/shader/programs/GaussianBlur.h"
#include "donner/gpu/shader/programs/ImageBlit.h"
#include "donner/gpu/shader/programs/Merge.h"
#include "donner/gpu/shader/programs/Morphology.h"
#include "donner/gpu/shader/programs/Offset.h"
#include "donner/gpu/shader/programs/SlugFill.h"
#include "donner/gpu/shader/programs/SlugGradient.h"
#include "donner/gpu/shader/programs/SlugMask.h"
#include "donner/gpu/shader/programs/SnapshotUnpremultiply.h"
#include "donner/gpu/shader/programs/SpecularLighting.h"
#include "donner/gpu/shader/programs/SubregionClip.h"
#include "donner/gpu/shader/programs/Tile.h"
#include "donner/gpu/shader/programs/Turbulence.h"

namespace {

namespace programs = donner::gpu::shader::programs;

/// Reads every byte of each projection so the linker keeps the whole artifact.
/// @param shader Frozen artifact to read.
uint32_t Checksum(const donner::gpu::shader::CompiledShaderView& shader) {
  uint32_t checksum = 0;
  const volatile char* wgsl = shader.wgsl.data();
  const volatile char* msl = shader.msl.data();
  const volatile uint32_t* spirv = shader.spirv.data();
  for (size_t i = 0; i < shader.wgsl.size(); ++i) checksum += static_cast<unsigned char>(wgsl[i]);
  for (size_t i = 0; i < shader.msl.size(); ++i) checksum += static_cast<unsigned char>(msl[i]);
  for (size_t i = 0; i < shader.spirv.size(); ++i) checksum += spirv[i];
  return checksum;
}

}  // namespace

int main() {
  const donner::gpu::shader::CompiledShaderView* const shaders[] = {
      &programs::CheckerboardShader(),
      &programs::CheckerboardNativeShader(),
      &programs::ColorSpaceConvertShader(),
      &programs::ColorSpaceConvertNativeShader(),
      &programs::ComponentTransferShader(),
      &programs::ComponentTransferNativeShader(),
      &programs::CompositeShader(),
      &programs::CompositeNativeShader(),
      &programs::ConvolveMatrixShader(),
      &programs::ConvolveMatrixNativeShader(),
      &programs::DiffuseLightingShader(),
      &programs::DiffuseLightingNativeShader(),
      &programs::DisplacementMapShader(),
      &programs::DisplacementMapNativeShader(),
      &programs::DropShadowShader(),
      &programs::DropShadowNativeShader(),
      &programs::FilterBlendShader(),
      &programs::FilterBlendNativeShader(),
      &programs::FilterColorMatrixShader(),
      &programs::FilterColorMatrixNativeShader(),
      &programs::FilterImageShader(),
      &programs::FilterImageNativeShader(),
      &programs::FilterResolveShader(),
      &programs::FilterResolveNativeShader(),
      &programs::FloodShader(),
      &programs::FloodNativeShader(),
      &programs::GaussianBlurShader(),
      &programs::GaussianBlurNativeShader(),
      &programs::ImageBlitShader(),
      &programs::ImageBlitNativeShader(),
      &programs::MergeShader(),
      &programs::MergeNativeShader(),
      &programs::MorphologyShader(),
      &programs::MorphologyNativeShader(),
      &programs::OffsetShader(),
      &programs::OffsetNativeShader(),
      &programs::SlugFillShader(),
      &programs::SlugFillNativeShader(),
      &programs::SlugGradientShader(),
      &programs::SlugGradientNativeShader(),
      &programs::SlugMaskShader(),
      &programs::SlugMaskNativeShader(),
      &programs::SnapshotUnpremultiplyShader(),
      &programs::SnapshotUnpremultiplyNativeShader(),
      &programs::SpecularLightingShader(),
      &programs::SpecularLightingNativeShader(),
      &programs::SubregionClipShader(),
      &programs::SubregionClipNativeShader(),
      &programs::TileShader(),
      &programs::TileNativeShader(),
      &programs::TurbulenceShader(),
      &programs::TurbulenceNativeShader(),
  };
  uint32_t checksum = 0;
  for (const donner::gpu::shader::CompiledShaderView* shader : shaders) {
    checksum += Checksum(*shader);
  }
  return checksum == 0 ? 1 : 0;
}
