#pragma once
/// @file
/// Host layout and frozen shader projections for analytic clip-mask coverage.

#include <cstddef>
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"

namespace donner::gpu::shader::programs {

/// Host uniform layout consumed by the Slug mask program.
struct alignas(16) SlugMaskParams {
  float mvp[16];                 //!< Column-major transform to clip space.
  float viewport[2];             //!< Render-target extent in pixels.
  uint32_t fillRule;             //!< Zero for nonzero winding, one for even-odd.
  uint32_t hasClipMask;          //!< One intersects coverage with the nested clip mask.
  float gridYBase;               //!< Horizontal-band grid origin.
  float gridHStride;             //!< Horizontal-band grid spacing.
  uint32_t gridHBandCount;       //!< Horizontal-band grid cell count.
  float gridXBase;               //!< Vertical-band grid origin.
  float gridVStride;             //!< Vertical-band grid spacing.
  uint32_t gridVBandCount;       //!< Vertical-band grid cell count.
  uint32_t antialias;            //!< One enables analytic coverage; zero uses binary coverage.
  uint32_t _gridPad1;            //!< Padding to the bounding data.
  uint32_t boundingVertexCount;  //!< Number of convex bounding-polygon vertices, at most eight.
  uint32_t _boundingPad0;        //!< Bounding data padding.
  uint32_t _boundingPad1;        //!< Bounding data padding.
  uint32_t _boundingPad2;        //!< Bounding data padding.
  float boundingVertices[16];    //!< Four packed pairs of bounding vertices.
};

/// Host element layout of either band array.
struct SlugMaskBand {
  uint32_t curveStart;  //!< First reference in the axis curve-reference array.
  uint32_t curveCount;  //!< Number of references in this band.
};

static_assert(sizeof(SlugMaskParams) == 192);
static_assert(offsetof(SlugMaskParams, boundingVertices) == 128);
static_assert(sizeof(SlugMaskBand) == 8);

/// Returns WGSL and the reflected vertex/fragment interface for analytic clip masks.
/// Host path admission supplies bounded band/curve data and a convex bounding fan.
/// @return Stable view into a process-lifetime artifact.
const CompiledShaderView& SlugMaskShader();

/// Returns only the platform's native MSL or SPIR-V projection and reflected interface.
/// @return Stable view into a process-lifetime artifact.
const CompiledShaderView& SlugMaskNativeShader();

}  // namespace donner::gpu::shader::programs
