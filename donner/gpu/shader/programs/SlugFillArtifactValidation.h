#pragma once
/// @file
/// Checks the complete Slug host layout against the authored shader.
#include <cstddef>
#include <string_view>
#include <utility>

#include "donner/gpu/shader/programs/SlugFill.h"
namespace donner::gpu::shader::programs {
struct SlugFillField {
  std::string_view resource, name;
  uint32_t offset, size;
  ShaderScalarType scalar;
  uint8_t lanes;
  uint32_t count, stride;
  uint8_t columns;
  uint32_t matrixStride;
};
template <const CompiledShaderView& Shader>
consteval bool ValidateSlugFillResources() {
  struct Expected {
    std::string_view name;
    BindingType type;
    uint32_t size, alignment, stride;
    ShaderScalarType scalar;
    uint8_t lanes;
  };
  constexpr Expected buffers[] = {
      {"uniforms", BindingType::UniformBuffer, 448, 16, 0, ShaderScalarType::F32, 0},
      {"instances", BindingType::ReadOnlyStorageBuffer, 256, 16, 256, ShaderScalarType::F32, 0},
      {"bands", BindingType::ReadOnlyStorageBuffer, 8, 4, 8, ShaderScalarType::F32, 0},
      {"vBands", BindingType::ReadOnlyStorageBuffer, 8, 4, 8, ShaderScalarType::F32, 0},
      {"curveData", BindingType::ReadOnlyStorageBuffer, 4, 4, 4, ShaderScalarType::F32, 1},
      {"vCurveData", BindingType::ReadOnlyStorageBuffer, 4, 4, 4, ShaderScalarType::F32, 1},
      {"gridData", BindingType::ReadOnlyStorageBuffer, 4, 4, 4, ShaderScalarType::U32, 1},
      {"paintData", BindingType::ReadOnlyStorageBuffer, 16, 16, 16, ShaderScalarType::F32, 4}};
  for (const auto& b : buffers) {
    const auto* r = Shader.resource(b.name);
    if (!r || r->type != b.type || r->minSizeBytes != b.size || r->alignmentBytes != b.alignment ||
        r->runtimeArrayStrideBytes != b.stride) {
      return false;
    }
    if (r->runtimeArrayLanes != b.lanes ||
        (b.lanes != 0 && r->runtimeArrayScalarType != b.scalar)) {
      return false;
    }
  }
  return true;
}
template <const CompiledShaderView& Shader>
consteval bool ValidateSlugFillTextures() {
  constexpr std::pair<std::string_view, BindingType> entries[] = {
      {"patternTexture", BindingType::SampledTexture2dFloat},
      {"patternSampler", BindingType::FilteringSampler},
      {"clipMaskTexture", BindingType::SampledTexture2dUnfilterableFloat}};
  for (const auto& e : entries) {
    const auto* r = Shader.resource(e.first);
    if (!r || r->type != e.second) {
      return false;
    }
  }
  return true;
}
template <const CompiledShaderView& Shader>
consteval bool ValidateSlugFillArtifact() {
  constexpr SlugFillField fields[] = {
      {"bands", "curveStart", offsetof(SlugFillBand, curveStart), sizeof(SlugFillBand::curveStart),
       ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"bands", "curveCount", offsetof(SlugFillBand, curveCount), sizeof(SlugFillBand::curveCount),
       ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"vBands", "curveStart", offsetof(SlugFillBand, curveStart), sizeof(SlugFillBand::curveStart),
       ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"vBands", "curveCount", offsetof(SlugFillBand, curveCount), sizeof(SlugFillBand::curveCount),
       ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "mvp", offsetof(SlugFillParams, mvp), sizeof(SlugFillParams::mvp),
       ShaderScalarType::F32, 4, 0, 0, 4, 16},
      {"uniforms", "patternFromPath", offsetof(SlugFillParams, patternFromPath),
       sizeof(SlugFillParams::patternFromPath), ShaderScalarType::F32, 4, 0, 0, 4, 16},
      {"uniforms", "viewport", offsetof(SlugFillParams, viewport), sizeof(SlugFillParams::viewport),
       ShaderScalarType::F32, 2, 0, 0, 0, 0},
      {"uniforms", "tileSize", offsetof(SlugFillParams, tileSize), sizeof(SlugFillParams::tileSize),
       ShaderScalarType::F32, 2, 0, 0, 0, 0},
      {"uniforms", "hasClipPolygon", offsetof(SlugFillParams, hasClipPolygon),
       sizeof(SlugFillParams::hasClipPolygon), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "hasClipMask", offsetof(SlugFillParams, hasClipMask),
       sizeof(SlugFillParams::hasClipMask), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "antialias", offsetof(SlugFillParams, antialias),
       sizeof(SlugFillParams::antialias), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "_pad1", offsetof(SlugFillParams, _pad1), sizeof(SlugFillParams::_pad1),
       ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "clipPolygonPlanes", offsetof(SlugFillParams, clipPolygonPlanes),
       sizeof(SlugFillParams::clipPolygonPlanes), ShaderScalarType::F32, 4, 4, 16, 0, 0},
      {"uniforms", "color", offsetof(SlugFillParams, color), sizeof(SlugFillParams::color),
       ShaderScalarType::F32, 4, 0, 0, 0, 0},
      {"uniforms", "fillRule", offsetof(SlugFillParams, fillRule), sizeof(SlugFillParams::fillRule),
       ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "paintMode", offsetof(SlugFillParams, paintMode),
       sizeof(SlugFillParams::paintMode), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "patternOpacity", offsetof(SlugFillParams, patternOpacity),
       sizeof(SlugFillParams::patternOpacity), ShaderScalarType::F32, 1, 0, 0, 0, 0},
      {"uniforms", "_pad2", offsetof(SlugFillParams, _pad2), sizeof(SlugFillParams::_pad2),
       ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "yBase", offsetof(SlugFillParams, gridYBase), sizeof(SlugFillParams::gridYBase),
       ShaderScalarType::F32, 1, 0, 0, 0, 0},
      {"uniforms", "hStride", offsetof(SlugFillParams, gridHStride),
       sizeof(SlugFillParams::gridHStride), ShaderScalarType::F32, 1, 0, 0, 0, 0},
      {"uniforms", "hBandCount", offsetof(SlugFillParams, gridHBandCount),
       sizeof(SlugFillParams::gridHBandCount), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "xBase", offsetof(SlugFillParams, gridXBase), sizeof(SlugFillParams::gridXBase),
       ShaderScalarType::F32, 1, 0, 0, 0, 0},
      {"uniforms", "vStride", offsetof(SlugFillParams, gridVStride),
       sizeof(SlugFillParams::gridVStride), ShaderScalarType::F32, 1, 0, 0, 0, 0},
      {"uniforms", "vBandCount", offsetof(SlugFillParams, gridVBandCount),
       sizeof(SlugFillParams::gridVBandCount), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "boundingVertexCount", offsetof(SlugFillParams, boundingVertexCount),
       sizeof(SlugFillParams::boundingVertexCount), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "_gridPad0", offsetof(SlugFillParams, _gridPad0),
       sizeof(SlugFillParams::_gridPad0), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "bandBase", offsetof(SlugFillParams, bandBase), sizeof(SlugFillParams::bandBase),
       ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "curveBase", offsetof(SlugFillParams, curveBase),
       sizeof(SlugFillParams::curveBase), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "vBandBase", offsetof(SlugFillParams, vBandBase),
       sizeof(SlugFillParams::vBandBase), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "vCurveBase", offsetof(SlugFillParams, vCurveBase),
       sizeof(SlugFillParams::vCurveBase), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "hGridBase", offsetof(SlugFillParams, hGridBase),
       sizeof(SlugFillParams::hGridBase), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "vGridBase", offsetof(SlugFillParams, vGridBase),
       sizeof(SlugFillParams::vGridBase), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "hRefsBase", offsetof(SlugFillParams, hRefsBase),
       sizeof(SlugFillParams::hRefsBase), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "vRefsBase", offsetof(SlugFillParams, vRefsBase),
       sizeof(SlugFillParams::vRefsBase), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "boundingVertices", offsetof(SlugFillParams, boundingVertices),
       sizeof(SlugFillParams::boundingVertices), ShaderScalarType::F32, 4, 4, 16, 0, 0},
      {"uniforms", "clipRect", offsetof(SlugFillParams, clipRect), sizeof(SlugFillParams::clipRect),
       ShaderScalarType::F32, 4, 0, 0, 0, 0},
      {"uniforms", "clipRectActive", offsetof(SlugFillParams, clipRectActive),
       sizeof(SlugFillParams::clipRectActive), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "paintBase", offsetof(SlugFillParams, paintBase),
       sizeof(SlugFillParams::paintBase), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "gradientSpread", offsetof(SlugFillParams, gradientSpread),
       sizeof(SlugFillParams::gradientSpread), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "gradientStopCount", offsetof(SlugFillParams, gradientStopCount),
       sizeof(SlugFillParams::gradientStopCount), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"uniforms", "pathFromPixel", offsetof(SlugFillParams, pathFromPixel),
       sizeof(SlugFillParams::pathFromPixel), ShaderScalarType::F32, 4, 0, 0, 0, 0},
      {"uniforms", "pixelOrigin", offsetof(SlugFillParams, pixelOrigin),
       sizeof(SlugFillParams::pixelOrigin), ShaderScalarType::F32, 2, 0, 0, 0, 0},
      {"uniforms", "pathOffset", offsetof(SlugFillParams, pathOffset),
       sizeof(SlugFillParams::pathOffset), ShaderScalarType::F32, 2, 0, 0, 0, 0},
      {"instances", "transform.row0", offsetof(SlugFillInstance, transformRow0),
       sizeof(SlugFillInstance::transformRow0), ShaderScalarType::F32, 4, 0, 0, 0, 0},
      {"instances", "transform.row1", offsetof(SlugFillInstance, transformRow1),
       sizeof(SlugFillInstance::transformRow1), ShaderScalarType::F32, 4, 0, 0, 0, 0},
      {"instances", "color", offsetof(SlugFillInstance, color), sizeof(SlugFillInstance::color),
       ShaderScalarType::F32, 4, 0, 0, 0, 0},
      {"instances", "fillRule", offsetof(SlugFillInstance, fillRule),
       sizeof(SlugFillInstance::fillRule), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "paintMode", offsetof(SlugFillInstance, paintMode),
       sizeof(SlugFillInstance::paintMode), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "patternOpacity", offsetof(SlugFillInstance, patternOpacity),
       sizeof(SlugFillInstance::patternOpacity), ShaderScalarType::F32, 1, 0, 0, 0, 0},
      {"instances", "_pad0", offsetof(SlugFillInstance, _pad0), sizeof(SlugFillInstance::_pad0),
       ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "yBase", offsetof(SlugFillInstance, gridYBase),
       sizeof(SlugFillInstance::gridYBase), ShaderScalarType::F32, 1, 0, 0, 0, 0},
      {"instances", "hStride", offsetof(SlugFillInstance, gridHStride),
       sizeof(SlugFillInstance::gridHStride), ShaderScalarType::F32, 1, 0, 0, 0, 0},
      {"instances", "hBandCount", offsetof(SlugFillInstance, gridHBandCount),
       sizeof(SlugFillInstance::gridHBandCount), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "xBase", offsetof(SlugFillInstance, gridXBase),
       sizeof(SlugFillInstance::gridXBase), ShaderScalarType::F32, 1, 0, 0, 0, 0},
      {"instances", "vStride", offsetof(SlugFillInstance, gridVStride),
       sizeof(SlugFillInstance::gridVStride), ShaderScalarType::F32, 1, 0, 0, 0, 0},
      {"instances", "vBandCount", offsetof(SlugFillInstance, gridVBandCount),
       sizeof(SlugFillInstance::gridVBandCount), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "pixelOrigin", offsetof(SlugFillInstance, pixelOrigin),
       sizeof(SlugFillInstance::pixelOrigin), ShaderScalarType::F32, 2, 0, 0, 0, 0},
      {"instances", "boundingVertexCount", offsetof(SlugFillInstance, boundingVertexCount),
       sizeof(SlugFillInstance::boundingVertexCount), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "pixelMappingSource", offsetof(SlugFillInstance, pixelMappingSource),
       sizeof(SlugFillInstance::pixelMappingSource), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "pathOffset", offsetof(SlugFillInstance, pathOffset),
       sizeof(SlugFillInstance::pathOffset), ShaderScalarType::F32, 2, 0, 0, 0, 0},
      {"instances", "boundingVertices", offsetof(SlugFillInstance, boundingVertices),
       sizeof(SlugFillInstance::boundingVertices), ShaderScalarType::F32, 4, 4, 16, 0, 0},
      {"instances", "bandBase", offsetof(SlugFillInstance, bandBase),
       sizeof(SlugFillInstance::bandBase), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "curveBase", offsetof(SlugFillInstance, curveBase),
       sizeof(SlugFillInstance::curveBase), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "vBandBase", offsetof(SlugFillInstance, vBandBase),
       sizeof(SlugFillInstance::vBandBase), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "vCurveBase", offsetof(SlugFillInstance, vCurveBase),
       sizeof(SlugFillInstance::vCurveBase), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "hGridBase", offsetof(SlugFillInstance, hGridBase),
       sizeof(SlugFillInstance::hGridBase), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "vGridBase", offsetof(SlugFillInstance, vGridBase),
       sizeof(SlugFillInstance::vGridBase), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "hRefsBase", offsetof(SlugFillInstance, hRefsBase),
       sizeof(SlugFillInstance::hRefsBase), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "vRefsBase", offsetof(SlugFillInstance, vRefsBase),
       sizeof(SlugFillInstance::vRefsBase), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "clipRect", offsetof(SlugFillInstance, clipRect),
       sizeof(SlugFillInstance::clipRect), ShaderScalarType::F32, 4, 0, 0, 0, 0},
      {"instances", "clipRectActive", offsetof(SlugFillInstance, clipRectActive),
       sizeof(SlugFillInstance::clipRectActive), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "paintBase", offsetof(SlugFillInstance, paintBase),
       sizeof(SlugFillInstance::paintBase), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "gradientSpread", offsetof(SlugFillInstance, gradientSpread),
       sizeof(SlugFillInstance::gradientSpread), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "gradientStopCount", offsetof(SlugFillInstance, gradientStopCount),
       sizeof(SlugFillInstance::gradientStopCount), ShaderScalarType::U32, 1, 0, 0, 0, 0},
      {"instances", "pathFromPixel", offsetof(SlugFillInstance, pathFromPixel),
       sizeof(SlugFillInstance::pathFromPixel), ShaderScalarType::F32, 4, 0, 0, 0, 0},
  };
  for (const auto& f : fields) {
    if (!Shader.matchesMember(f.resource, f.name, f.offset, f.size, f.scalar, f.lanes, f.count,
                              f.stride, f.columns, f.matrixStride)) {
      return false;
    }
  }
  if (Shader.entryPoints.size() != 4 || Shader.resources.size() != 11) {
    return false;
  }
  for (size_t i = 0; i < 4; ++i) {
    if (Shader.entryPoints[i].stage != (i < 2 ? ShaderStage::Vertex : ShaderStage::Fragment)) {
      return false;
    }
  }
  return ValidateSlugFillResources<Shader>() && ValidateSlugFillTextures<Shader>();
}
}  // namespace donner::gpu::shader::programs
