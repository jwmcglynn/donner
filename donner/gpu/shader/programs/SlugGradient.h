#pragma once
/// @file
/// Shared gradient host layout and selected frozen shader projections.
#include <cstddef>
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Maximum admitted gradient stops.
inline constexpr uint32_t kSlugGradientMaxStops = 16;
/// Host uniform layout for dedicated Slug gradients.
struct alignas(16) SlugGradientParams {
  float mvp[16];                                //!< Column-major transform from path to clip space.
  float viewport[2];                            //!< Render-target extent in pixels.
  uint32_t fillRule;                            //!< Zero for nonzero winding, one for even-odd.
  uint32_t spreadMode;                          //!< Zero clamps, one reflects, two repeats.
  float row0[4];                                //!< First affine row of gradientFromPath.
  float row1[4];                                //!< Second affine row of gradientFromPath.
  float startGrad[2];                           //!< Linear-gradient start in gradient space.
  float endGrad[2];                             //!< Linear-gradient end in gradient space.
  float radialCenter[2];                        //!< Outer-circle center in gradient space.
  float radialFocal[2];                         //!< Focal-circle center in gradient space.
  float radialRadius;                           //!< Outer-circle radius.
  float radialFocalRadius;                      //!< Focal-circle radius.
  uint32_t gradientKind;                        //!< Zero selects linear, one selects radial.
  uint32_t stopCount;                           //!< Admitted stop count, at most sixteen.
  float stopColors[kSlugGradientMaxStops * 4];  //!< Straight-alpha RGBA stop colors.
  float stopOffsets[4 * 4];                     //!< Stop offsets packed four per vector.
  uint32_t hasClipPolygon;                      //!< One enables the convex clip half-planes.
  uint32_t hasClipMask;                         //!< One intersects the path-clip texture.
  uint32_t antialias;             //!< One enables analytic coverage; zero uses binary coverage.
  uint32_t _clipPad2;             //!< Reserved layout padding.
  float gridYBase;                //!< Horizontal-band grid origin.
  float gridHStride;              //!< Horizontal-band grid spacing.
  uint32_t gridHBandCount;        //!< Horizontal-band grid cell count.
  float gridXBase;                //!< Vertical-band grid origin.
  float gridVStride;              //!< Vertical-band grid spacing.
  uint32_t gridVBandCount;        //!< Vertical-band grid cell count.
  uint32_t _gridPad0;             //!< Reserved layout padding.
  uint32_t _gridPad1;             //!< Reserved layout padding.
  float clipPolygonPlanes[16];    //!< Four inward-facing clip half-planes in viewport pixels.
  uint32_t boundingVertexCount;   //!< Convex bounding-polygon vertex count, at most eight.
  uint32_t _boundingPad0;         //!< Reserved layout padding.
  uint32_t _boundingPad1;         //!< Reserved layout padding.
  uint32_t _boundingPad2;         //!< Reserved layout padding.
  float boundingVertices[4 * 4];  //!< Four packed pairs of bounding vertices.
};
/// Host element of either gradient band array.
struct SlugGradientBand {
  uint32_t curveStart;  //!< First curve reference.
  uint32_t curveCount;  //!< Number of curve references.
};
static_assert(sizeof(SlugGradientParams) == 672);
static_assert(sizeof(SlugGradientBand) == 8);
/// Returns the WGSL gradient artifact.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& SlugGradientShader();
/// Returns the platform-native gradient artifact.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& SlugGradientNativeShader();
}  // namespace donner::gpu::shader::programs
