#pragma once
/// @file
/// Compile-time host-layout checks shared by Slug mask projections.

#include "donner/gpu/shader/programs/SlugMask.h"

namespace donner::gpu::shader::programs {

/// Checks every mask buffer field and resource against the host interface.
/// @tparam shader Static reflected shader interface.
template <const CompiledShaderView& shader>
consteval bool ValidateSlugMaskArtifact() {
  static_assert(shader.entryPoints.size() == 2);
  static_assert(shader.entryPoints[0].stage == ShaderStage::Vertex);
  static_assert(shader.entryPoints[1].stage == ShaderStage::Fragment);
  static_assert(shader.resources.size() == 10);
  constexpr const auto* uniforms = shader.resource("uniforms");
  static_assert(uniforms != nullptr && uniforms->type == BindingType::UniformBuffer);
  static_assert(uniforms->minSizeBytes == sizeof(SlugMaskParams));
  static_assert(uniforms->alignmentBytes == alignof(SlugMaskParams));
  static_assert(shader.resource("clipMaskTexture") != nullptr);
  static_assert(shader.resource("clipMaskTexture")->type ==
                BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(shader.matchesMember("uniforms", "mvp", offsetof(SlugMaskParams, mvp),
                                     sizeof(SlugMaskParams::mvp), ShaderScalarType::F32, 4, 0, 0, 4,
                                     16));
  static_assert(shader.matchesMember("uniforms", "viewport", offsetof(SlugMaskParams, viewport),
                                     sizeof(SlugMaskParams::viewport), ShaderScalarType::F32, 2));
  static_assert(shader.matchesMember("uniforms", "fillRule", offsetof(SlugMaskParams, fillRule),
                                     sizeof(SlugMaskParams::fillRule), ShaderScalarType::U32));
  static_assert(shader.matchesMember("uniforms", "hasClipMask",
                                     offsetof(SlugMaskParams, hasClipMask),
                                     sizeof(SlugMaskParams::hasClipMask), ShaderScalarType::U32));
  static_assert(shader.matchesMember("uniforms", "yBase", offsetof(SlugMaskParams, gridYBase),
                                     sizeof(SlugMaskParams::gridYBase), ShaderScalarType::F32));
  static_assert(shader.matchesMember("uniforms", "hStride", offsetof(SlugMaskParams, gridHStride),
                                     sizeof(SlugMaskParams::gridHStride), ShaderScalarType::F32));
  static_assert(
      shader.matchesMember("uniforms", "hBandCount", offsetof(SlugMaskParams, gridHBandCount),
                           sizeof(SlugMaskParams::gridHBandCount), ShaderScalarType::U32));
  static_assert(shader.matchesMember("uniforms", "xBase", offsetof(SlugMaskParams, gridXBase),
                                     sizeof(SlugMaskParams::gridXBase), ShaderScalarType::F32));
  static_assert(shader.matchesMember("uniforms", "vStride", offsetof(SlugMaskParams, gridVStride),
                                     sizeof(SlugMaskParams::gridVStride), ShaderScalarType::F32));
  static_assert(
      shader.matchesMember("uniforms", "vBandCount", offsetof(SlugMaskParams, gridVBandCount),
                           sizeof(SlugMaskParams::gridVBandCount), ShaderScalarType::U32));
  static_assert(shader.matchesMember("uniforms", "antialias", offsetof(SlugMaskParams, antialias),
                                     sizeof(SlugMaskParams::antialias), ShaderScalarType::U32));
  static_assert(shader.matchesMember("uniforms", "_gridPad1", offsetof(SlugMaskParams, _gridPad1),
                                     sizeof(SlugMaskParams::_gridPad1), ShaderScalarType::U32));
  static_assert(shader.matchesMember(
      "uniforms", "boundingVertexCount", offsetof(SlugMaskParams, boundingVertexCount),
      sizeof(SlugMaskParams::boundingVertexCount), ShaderScalarType::U32));
  static_assert(shader.matchesMember("uniforms", "_boundingPad0",
                                     offsetof(SlugMaskParams, _boundingPad0),
                                     sizeof(SlugMaskParams::_boundingPad0), ShaderScalarType::U32));
  static_assert(shader.matchesMember("uniforms", "_boundingPad1",
                                     offsetof(SlugMaskParams, _boundingPad1),
                                     sizeof(SlugMaskParams::_boundingPad1), ShaderScalarType::U32));
  static_assert(shader.matchesMember("uniforms", "_boundingPad2",
                                     offsetof(SlugMaskParams, _boundingPad2),
                                     sizeof(SlugMaskParams::_boundingPad2), ShaderScalarType::U32));
  static_assert(shader.matchesMember(
      "uniforms", "boundingVertices", offsetof(SlugMaskParams, boundingVertices),
      sizeof(SlugMaskParams::boundingVertices), ShaderScalarType::F32, 4, 4, 16));
  static_assert(
      shader.matchesMember("uniforms", "pathFromPixel", offsetof(SlugMaskParams, pathFromPixel),
                           sizeof(SlugMaskParams::pathFromPixel), ShaderScalarType::F32, 4));
  static_assert(
      shader.matchesMember("uniforms", "pixelOrigin", offsetof(SlugMaskParams, pixelOrigin),
                           sizeof(SlugMaskParams::pixelOrigin), ShaderScalarType::F32, 2));
  static_assert(shader.matchesMember("uniforms", "pathOffset", offsetof(SlugMaskParams, pathOffset),
                                     sizeof(SlugMaskParams::pathOffset), ShaderScalarType::F32, 2));
  static_assert(shader.resource("bands") != nullptr);
  static_assert(shader.resource("bands")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("bands")->runtimeArrayStrideBytes == sizeof(SlugMaskBand));
  static_assert(shader.matchesMember("bands", "curveStart", offsetof(SlugMaskBand, curveStart),
                                     sizeof(SlugMaskBand::curveStart), ShaderScalarType::U32));
  static_assert(shader.matchesMember("bands", "curveCount", offsetof(SlugMaskBand, curveCount),
                                     sizeof(SlugMaskBand::curveCount), ShaderScalarType::U32));
  static_assert(shader.resource("vBands") != nullptr);
  static_assert(shader.resource("vBands")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("vBands")->runtimeArrayStrideBytes == sizeof(SlugMaskBand));
  static_assert(shader.matchesMember("vBands", "curveStart", offsetof(SlugMaskBand, curveStart),
                                     sizeof(SlugMaskBand::curveStart), ShaderScalarType::U32));
  static_assert(shader.matchesMember("vBands", "curveCount", offsetof(SlugMaskBand, curveCount),
                                     sizeof(SlugMaskBand::curveCount), ShaderScalarType::U32));
  static_assert(shader.resource("curveData") != nullptr);
  static_assert(shader.resource("curveData")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("curveData")->runtimeArrayStrideBytes == 4);
  static_assert(shader.resource("curveData")->runtimeArrayScalarType == ShaderScalarType::F32);
  static_assert(shader.resource("curveData")->runtimeArrayLanes == 1);
  static_assert(shader.resource("vCurveData") != nullptr);
  static_assert(shader.resource("vCurveData")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("vCurveData")->runtimeArrayStrideBytes == 4);
  static_assert(shader.resource("vCurveData")->runtimeArrayScalarType == ShaderScalarType::F32);
  static_assert(shader.resource("vCurveData")->runtimeArrayLanes == 1);
  static_assert(shader.resource("hBandGrid") != nullptr);
  static_assert(shader.resource("hBandGrid")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("hBandGrid")->runtimeArrayStrideBytes == 4);
  static_assert(shader.resource("hBandGrid")->runtimeArrayScalarType == ShaderScalarType::U32);
  static_assert(shader.resource("hBandGrid")->runtimeArrayLanes == 1);
  static_assert(shader.resource("vBandGrid") != nullptr);
  static_assert(shader.resource("vBandGrid")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("vBandGrid")->runtimeArrayStrideBytes == 4);
  static_assert(shader.resource("vBandGrid")->runtimeArrayScalarType == ShaderScalarType::U32);
  static_assert(shader.resource("vBandGrid")->runtimeArrayLanes == 1);
  static_assert(shader.resource("hCurveIndices") != nullptr);
  static_assert(shader.resource("hCurveIndices")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("hCurveIndices")->runtimeArrayStrideBytes == 4);
  static_assert(shader.resource("hCurveIndices")->runtimeArrayScalarType == ShaderScalarType::U32);
  static_assert(shader.resource("hCurveIndices")->runtimeArrayLanes == 1);
  static_assert(shader.resource("vCurveIndices") != nullptr);
  static_assert(shader.resource("vCurveIndices")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("vCurveIndices")->runtimeArrayStrideBytes == 4);
  static_assert(shader.resource("vCurveIndices")->runtimeArrayScalarType == ShaderScalarType::U32);
  static_assert(shader.resource("vCurveIndices")->runtimeArrayLanes == 1);
  return true;
}

}  // namespace donner::gpu::shader::programs
