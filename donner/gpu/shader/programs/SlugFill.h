#pragma once
/// @file
/// Shared Slug fill layouts and frozen shader interfaces.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Per-draw transform, paint, geometry and clip parameters.
struct alignas(16) SlugFillParams {
  float mvp[16];                 //!< Column-major clip-from-target matrix.
  float patternFromPath[16];     //!< Column-major pattern-from-path matrix.
  float viewport[2];             //!< Target dimensions in pixels.
  float tileSize[2];             //!< Pattern tile dimensions.
  uint32_t hasClipPolygon;       //!< Enables the four clip half-planes.
  uint32_t hasClipMask;          //!< Enables sampled clip coverage.
  uint32_t antialias;            //!< Zero selects binary winding coverage.
  uint32_t _pad1;                //!< Reserved layout padding.
  float clipPolygonPlanes[16];   //!< Four inward pixel-space half-planes.
  float color[4];                //!< Premultiplied fill color.
  uint32_t fillRule;             //!< Zero nonzero, one even-odd.
  uint32_t paintMode;            //!< Solid, pattern, linear or radial paint selector.
  float patternOpacity;          //!< Pattern alpha multiplier.
  uint32_t _pad2;                //!< Reserved layout padding.
  float gridYBase;               //!< Horizontal-band grid origin.
  float gridHStride;             //!< Horizontal-band grid step.
  uint32_t gridHBandCount;       //!< Horizontal-band count.
  float gridXBase;               //!< Vertical-band grid origin.
  float gridVStride;             //!< Vertical-band grid step.
  uint32_t gridVBandCount;       //!< Vertical-band count.
  uint32_t boundingVertexCount;  //!< Number of convex enclosure vertices.
  uint32_t _gridPad0;            //!< Reserved layout padding.
  uint32_t bandBase;             //!< Base element in horizontal bands.
  uint32_t curveBase;            //!< Base element in horizontal curve scalars.
  uint32_t vBandBase;            //!< Base element in vertical bands.
  uint32_t vCurveBase;           //!< Base element in vertical curve scalars.
  uint32_t hGridBase;            //!< Base element in horizontal grid.
  uint32_t vGridBase;            //!< Base element in vertical grid.
  uint32_t hRefsBase;            //!< Base element in horizontal curve references.
  uint32_t vRefsBase;            //!< Base element in vertical curve references.
  float boundingVertices[16];    //!< Two path-space corners per vec4.
  float clipRect[4];             //!< Half-open pixel-space clip rectangle.
  uint32_t clipRectActive;       //!< Enables the instance clip rectangle.
  uint32_t paintBase;            //!< Base element in gradient paint rows.
  uint32_t gradientSpread;       //!< Pad, reflect or repeat spread selector.
  uint32_t gradientStopCount;    //!< Number of active gradient stops.
};
static_assert(sizeof(SlugFillParams) == 416);
/// One painter-ordered instance, including its nested transform and bounding polygon.
struct alignas(16) SlugFillInstance {
  float transformRow0[4];         //!< First affine row: a, c, e, padding.
  float transformRow1[4];         //!< Second affine row: b, d, f, padding.
  float color[4];                 //!< Premultiplied fill color.
  uint32_t fillRule;              //!< Zero nonzero, one even-odd.
  uint32_t paintMode;             //!< Solid, pattern, linear or radial paint selector.
  float patternOpacity;           //!< Pattern alpha multiplier.
  uint32_t _pad0;                 //!< Reserved layout padding.
  float gridYBase;                //!< Horizontal-band grid origin.
  float gridHStride;              //!< Horizontal-band grid step.
  uint32_t gridHBandCount;        //!< Horizontal-band count.
  float gridXBase;                //!< Vertical-band grid origin.
  float gridVStride;              //!< Vertical-band grid step.
  uint32_t gridVBandCount;        //!< Vertical-band count.
  uint32_t _gridPad0;             //!< Reserved layout padding.
  uint32_t _gridPad1;             //!< Reserved layout padding.
  uint32_t boundingVertexCount;   //!< Number of convex enclosure vertices.
  uint32_t _boundingPad0;         //!< Reserved layout padding.
  uint32_t _boundingPad1;         //!< Reserved layout padding.
  uint32_t _boundingPad2;         //!< Reserved layout padding.
  float boundingVertices[4 * 4];  //!< Two path-space corners per vec4.
  uint32_t bandBase;              //!< Base element in horizontal bands.
  uint32_t curveBase;             //!< Base element in horizontal curve scalars.
  uint32_t vBandBase;             //!< Base element in vertical bands.
  uint32_t vCurveBase;            //!< Base element in vertical curve scalars.
  uint32_t hGridBase;             //!< Base element in horizontal grid.
  uint32_t vGridBase;             //!< Base element in vertical grid.
  uint32_t hRefsBase;             //!< Base element in horizontal curve references.
  uint32_t vRefsBase;             //!< Base element in vertical curve references.
  float clipRect[4];              //!< Half-open pixel-space clip rectangle.
  uint32_t clipRectActive;        //!< Enables the instance clip rectangle.
  uint32_t paintBase;             //!< Base element in gradient paint rows.
  uint32_t gradientSpread;        //!< Pad, reflect or repeat spread selector.
  uint32_t gradientStopCount;     //!< Number of active gradient stops.
  float _padTail[4];              //!< Reserved layout padding.
};
static_assert(sizeof(SlugFillInstance) == 256);
/// A band indexes the corresponding curve-reference range.
struct SlugFillBand {
  uint32_t curveStart;  //!< First curve-reference element.
  uint32_t curveCount;  //!< Number of curve references.
};
/// Returns a stable view of the authored WGSL and reflected interface.
const CompiledShaderView& SlugFillShader();
/// Returns a stable view containing only the platform-native projection.
const CompiledShaderView& SlugFillNativeShader();
}  // namespace donner::gpu::shader::programs
