#pragma once
/// @file
/// Image-blit uniform layout and precompiled shader projections.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Host layout for quad geometry, sampling, masking and compositing.
struct alignas(16) ImageBlitParams {
  float mvp[16];             //!< Column-major clip-from-target transform.
  float destRect[4];         //!< Target-pixel rectangle.
  float srcRect[4];          //!< Normalized source UV rectangle.
  float targetSize[2];       //!< Target dimensions in pixels.
  float opacity;             //!< Overall opacity multiplier.
  uint32_t sourceIsPremult;  //!< Nonzero for premultiplied source texels.
  uint32_t
      maskMode;  //!< Zero disables masks, one sRGB luminance, two alpha, three linear luminance.
  uint32_t applyMaskBounds;             //!< Nonzero enables the target-pixel bounds check.
  uint32_t paddingBeforeMaskBounds[2];  //!< Aligns the following vector to 16 bytes.
  float maskBounds[4];                  //!< Half-open target-pixel mask rectangle.
  uint32_t blendMode;                   //!< Zero source-over, one through fifteen CSS blend modes.
  uint32_t hasClipMask;                 //!< Nonzero enables the path-clip texture.
  uint32_t samplingMode;  //!< Zero linear, one nearest, two pixelated, three cubic, four anchored.
  uint32_t blendPadding;  //!< Reserved shader padding.
  float pixelatedScale[2];  //!< Device pixels per source texel.
  int32_t texelWholeX;      //!< Anchored sampling: whole texel x of the anchor pixel.
  int32_t texelWholeY;      //!< Anchored sampling: whole texel y of the anchor pixel.
  /// Nearest sampling: x, y and constant terms of the texel x. Anchored sampling: x and y terms,
  /// the anchor's fractional texel x, and the anchor pixel's x.
  float texelFromTargetX[4];
  /// The same terms for the texel y.
  float texelFromTargetY[4];
};
static_assert(sizeof(ImageBlitParams) == 208);
/// Returns authored WGSL and reflected image-blit interfaces.
/// @return Stable view into a process-lifetime artifact.
const CompiledShaderView& ImageBlitShader();
/// Returns only the platform-native MSL or SPIR-V projection and reflection.
/// @return Stable view into a process-lifetime artifact.
const CompiledShaderView& ImageBlitNativeShader();
}  // namespace donner::gpu::shader::programs
