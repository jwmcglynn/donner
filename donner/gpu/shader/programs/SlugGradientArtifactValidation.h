#pragma once
/// @file
/// Complete host-layout verification for dedicated Slug gradients.
#include <cstddef>

#include "donner/gpu/shader/programs/SlugGradient.h"
namespace donner::gpu::shader::programs {
/// Verifies every host field, array shape and resource role.
/// @tparam shader Static compiled interface.
template <const CompiledShaderView& shader>
consteval bool ValidateSlugGradientArtifact() {
  static_assert(shader.entryPoints.size() == 2);
  static_assert(shader.entryPoints[0].stage == ShaderStage::Vertex);
  static_assert(shader.entryPoints[1].stage == ShaderStage::Fragment);
  static_assert(shader.resources.size() == 10);
  constexpr auto* uniforms = shader.resource("uniforms");
  static_assert(uniforms && uniforms->type == BindingType::UniformBuffer);
  static_assert(uniforms->minSizeBytes == sizeof(SlugGradientParams));
  static_assert(uniforms->alignmentBytes == alignof(SlugGradientParams));
  static_assert(shader.matchesMember("uniforms", "mvp", offsetof(SlugGradientParams, mvp),
                                     sizeof(SlugGradientParams::mvp), ShaderScalarType::F32, 4, 0,
                                     0, 4, 16));
  static_assert(shader.matchesMember("uniforms", "viewport", offsetof(SlugGradientParams, viewport),
                                     sizeof(SlugGradientParams::viewport), ShaderScalarType::F32,
                                     2));
  static_assert(shader.matchesMember("uniforms", "fillRule", offsetof(SlugGradientParams, fillRule),
                                     sizeof(SlugGradientParams::fillRule), ShaderScalarType::U32,
                                     1));
  static_assert(
      shader.matchesMember("uniforms", "spreadMode", offsetof(SlugGradientParams, spreadMode),
                           sizeof(SlugGradientParams::spreadMode), ShaderScalarType::U32, 1));
  static_assert(shader.matchesMember("uniforms", "row0", offsetof(SlugGradientParams, row0),
                                     sizeof(SlugGradientParams::row0), ShaderScalarType::F32, 4));
  static_assert(shader.matchesMember("uniforms", "row1", offsetof(SlugGradientParams, row1),
                                     sizeof(SlugGradientParams::row1), ShaderScalarType::F32, 4));
  static_assert(
      shader.matchesMember("uniforms", "startGrad", offsetof(SlugGradientParams, startGrad),
                           sizeof(SlugGradientParams::startGrad), ShaderScalarType::F32, 2));
  static_assert(shader.matchesMember("uniforms", "endGrad", offsetof(SlugGradientParams, endGrad),
                                     sizeof(SlugGradientParams::endGrad), ShaderScalarType::F32,
                                     2));
  static_assert(
      shader.matchesMember("uniforms", "radialCenter", offsetof(SlugGradientParams, radialCenter),
                           sizeof(SlugGradientParams::radialCenter), ShaderScalarType::F32, 2));
  static_assert(
      shader.matchesMember("uniforms", "radialFocal", offsetof(SlugGradientParams, radialFocal),
                           sizeof(SlugGradientParams::radialFocal), ShaderScalarType::F32, 2));
  static_assert(
      shader.matchesMember("uniforms", "radialRadius", offsetof(SlugGradientParams, radialRadius),
                           sizeof(SlugGradientParams::radialRadius), ShaderScalarType::F32, 1));
  static_assert(shader.matchesMember(
      "uniforms", "radialFocalRadius", offsetof(SlugGradientParams, radialFocalRadius),
      sizeof(SlugGradientParams::radialFocalRadius), ShaderScalarType::F32, 1));
  static_assert(
      shader.matchesMember("uniforms", "gradientKind", offsetof(SlugGradientParams, gradientKind),
                           sizeof(SlugGradientParams::gradientKind), ShaderScalarType::U32, 1));
  static_assert(
      shader.matchesMember("uniforms", "stopCount", offsetof(SlugGradientParams, stopCount),
                           sizeof(SlugGradientParams::stopCount), ShaderScalarType::U32, 1));
  static_assert(shader.matchesMember(
      "uniforms", "stopColors", offsetof(SlugGradientParams, stopColors),
      sizeof(SlugGradientParams::stopColors), ShaderScalarType::F32, 4, 16, 16));
  static_assert(shader.matchesMember(
      "uniforms", "stopOffsets", offsetof(SlugGradientParams, stopOffsets),
      sizeof(SlugGradientParams::stopOffsets), ShaderScalarType::F32, 4, 4, 16));
  static_assert(shader.matchesMember(
      "uniforms", "hasClipPolygon", offsetof(SlugGradientParams, hasClipPolygon),
      sizeof(SlugGradientParams::hasClipPolygon), ShaderScalarType::U32, 1));
  static_assert(
      shader.matchesMember("uniforms", "hasClipMask", offsetof(SlugGradientParams, hasClipMask),
                           sizeof(SlugGradientParams::hasClipMask), ShaderScalarType::U32, 1));
  static_assert(
      shader.matchesMember("uniforms", "antialias", offsetof(SlugGradientParams, antialias),
                           sizeof(SlugGradientParams::antialias), ShaderScalarType::U32, 1));
  static_assert(
      shader.matchesMember("uniforms", "_clipPad2", offsetof(SlugGradientParams, _clipPad2),
                           sizeof(SlugGradientParams::_clipPad2), ShaderScalarType::U32, 1));
  static_assert(shader.matchesMember("uniforms", "yBase", offsetof(SlugGradientParams, gridYBase),
                                     sizeof(SlugGradientParams::gridYBase), ShaderScalarType::F32,
                                     1));
  static_assert(
      shader.matchesMember("uniforms", "hStride", offsetof(SlugGradientParams, gridHStride),
                           sizeof(SlugGradientParams::gridHStride), ShaderScalarType::F32, 1));
  static_assert(
      shader.matchesMember("uniforms", "hBandCount", offsetof(SlugGradientParams, gridHBandCount),
                           sizeof(SlugGradientParams::gridHBandCount), ShaderScalarType::U32, 1));
  static_assert(shader.matchesMember("uniforms", "xBase", offsetof(SlugGradientParams, gridXBase),
                                     sizeof(SlugGradientParams::gridXBase), ShaderScalarType::F32,
                                     1));
  static_assert(
      shader.matchesMember("uniforms", "vStride", offsetof(SlugGradientParams, gridVStride),
                           sizeof(SlugGradientParams::gridVStride), ShaderScalarType::F32, 1));
  static_assert(
      shader.matchesMember("uniforms", "vBandCount", offsetof(SlugGradientParams, gridVBandCount),
                           sizeof(SlugGradientParams::gridVBandCount), ShaderScalarType::U32, 1));
  static_assert(
      shader.matchesMember("uniforms", "_gridPad0", offsetof(SlugGradientParams, _gridPad0),
                           sizeof(SlugGradientParams::_gridPad0), ShaderScalarType::U32, 1));
  static_assert(
      shader.matchesMember("uniforms", "_gridPad1", offsetof(SlugGradientParams, _gridPad1),
                           sizeof(SlugGradientParams::_gridPad1), ShaderScalarType::U32, 1));
  static_assert(shader.matchesMember(
      "uniforms", "clipPolygonPlanes", offsetof(SlugGradientParams, clipPolygonPlanes),
      sizeof(SlugGradientParams::clipPolygonPlanes), ShaderScalarType::F32, 4, 4, 16));
  static_assert(shader.matchesMember(
      "uniforms", "boundingVertexCount", offsetof(SlugGradientParams, boundingVertexCount),
      sizeof(SlugGradientParams::boundingVertexCount), ShaderScalarType::U32, 1));
  static_assert(
      shader.matchesMember("uniforms", "_boundingPad0", offsetof(SlugGradientParams, _boundingPad0),
                           sizeof(SlugGradientParams::_boundingPad0), ShaderScalarType::U32, 1));
  static_assert(
      shader.matchesMember("uniforms", "_boundingPad1", offsetof(SlugGradientParams, _boundingPad1),
                           sizeof(SlugGradientParams::_boundingPad1), ShaderScalarType::U32, 1));
  static_assert(
      shader.matchesMember("uniforms", "_boundingPad2", offsetof(SlugGradientParams, _boundingPad2),
                           sizeof(SlugGradientParams::_boundingPad2), ShaderScalarType::U32, 1));
  static_assert(shader.matchesMember(
      "uniforms", "boundingVertices", offsetof(SlugGradientParams, boundingVertices),
      sizeof(SlugGradientParams::boundingVertices), ShaderScalarType::F32, 4, 4, 16));
  static_assert(
      shader.matchesMember("uniforms", "pathFromPixel", offsetof(SlugGradientParams, pathFromPixel),
                           sizeof(SlugGradientParams::pathFromPixel), ShaderScalarType::F32, 4));
  static_assert(
      shader.matchesMember("uniforms", "pixelOrigin", offsetof(SlugGradientParams, pixelOrigin),
                           sizeof(SlugGradientParams::pixelOrigin), ShaderScalarType::F32, 2));
  static_assert(
      shader.matchesMember("uniforms", "pathOffset", offsetof(SlugGradientParams, pathOffset),
                           sizeof(SlugGradientParams::pathOffset), ShaderScalarType::F32, 2));
  static_assert(shader.resource("bands") &&
                shader.resource("bands")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("bands")->minSizeBytes == sizeof(SlugGradientBand));
  static_assert(shader.resource("bands")->alignmentBytes == alignof(SlugGradientBand));
  static_assert(shader.resource("bands")->runtimeArrayStrideBytes == sizeof(SlugGradientBand));
  static_assert(shader.resource("bands")->runtimeArrayLanes == 0);
  static_assert(shader.matchesMember("bands", "curveStart", offsetof(SlugGradientBand, curveStart),
                                     sizeof(SlugGradientBand::curveStart), ShaderScalarType::U32));
  static_assert(shader.matchesMember("bands", "curveCount", offsetof(SlugGradientBand, curveCount),
                                     sizeof(SlugGradientBand::curveCount), ShaderScalarType::U32));
  static_assert(shader.resource("vBands") &&
                shader.resource("vBands")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("vBands")->minSizeBytes == sizeof(SlugGradientBand));
  static_assert(shader.resource("vBands")->alignmentBytes == alignof(SlugGradientBand));
  static_assert(shader.resource("vBands")->runtimeArrayStrideBytes == sizeof(SlugGradientBand));
  static_assert(shader.resource("vBands")->runtimeArrayLanes == 0);
  static_assert(shader.matchesMember("vBands", "curveStart", offsetof(SlugGradientBand, curveStart),
                                     sizeof(SlugGradientBand::curveStart), ShaderScalarType::U32));
  static_assert(shader.matchesMember("vBands", "curveCount", offsetof(SlugGradientBand, curveCount),
                                     sizeof(SlugGradientBand::curveCount), ShaderScalarType::U32));
  static_assert(shader.resource("curveData") &&
                shader.resource("curveData")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("curveData")->minSizeBytes == 4);
  static_assert(shader.resource("curveData")->alignmentBytes == 4);
  static_assert(shader.resource("curveData")->runtimeArrayStrideBytes == 4);
  static_assert(shader.resource("curveData")->runtimeArrayLanes == 1);
  static_assert(shader.resource("curveData")->runtimeArrayScalarType == ShaderScalarType::F32);
  static_assert(shader.resource("vCurveData") &&
                shader.resource("vCurveData")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("vCurveData")->minSizeBytes == 4);
  static_assert(shader.resource("vCurveData")->alignmentBytes == 4);
  static_assert(shader.resource("vCurveData")->runtimeArrayStrideBytes == 4);
  static_assert(shader.resource("vCurveData")->runtimeArrayLanes == 1);
  static_assert(shader.resource("vCurveData")->runtimeArrayScalarType == ShaderScalarType::F32);
  static_assert(shader.resource("hBandGrid") &&
                shader.resource("hBandGrid")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("hBandGrid")->minSizeBytes == 4);
  static_assert(shader.resource("hBandGrid")->alignmentBytes == 4);
  static_assert(shader.resource("hBandGrid")->runtimeArrayStrideBytes == 4);
  static_assert(shader.resource("hBandGrid")->runtimeArrayLanes == 1);
  static_assert(shader.resource("hBandGrid")->runtimeArrayScalarType == ShaderScalarType::U32);
  static_assert(shader.resource("vBandGrid") &&
                shader.resource("vBandGrid")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("vBandGrid")->minSizeBytes == 4);
  static_assert(shader.resource("vBandGrid")->alignmentBytes == 4);
  static_assert(shader.resource("vBandGrid")->runtimeArrayStrideBytes == 4);
  static_assert(shader.resource("vBandGrid")->runtimeArrayLanes == 1);
  static_assert(shader.resource("vBandGrid")->runtimeArrayScalarType == ShaderScalarType::U32);
  static_assert(shader.resource("hCurveIndices") &&
                shader.resource("hCurveIndices")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("hCurveIndices")->minSizeBytes == 4);
  static_assert(shader.resource("hCurveIndices")->alignmentBytes == 4);
  static_assert(shader.resource("hCurveIndices")->runtimeArrayStrideBytes == 4);
  static_assert(shader.resource("hCurveIndices")->runtimeArrayLanes == 1);
  static_assert(shader.resource("hCurveIndices")->runtimeArrayScalarType == ShaderScalarType::U32);
  static_assert(shader.resource("vCurveIndices") &&
                shader.resource("vCurveIndices")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("vCurveIndices")->minSizeBytes == 4);
  static_assert(shader.resource("vCurveIndices")->alignmentBytes == 4);
  static_assert(shader.resource("vCurveIndices")->runtimeArrayStrideBytes == 4);
  static_assert(shader.resource("vCurveIndices")->runtimeArrayLanes == 1);
  static_assert(shader.resource("vCurveIndices")->runtimeArrayScalarType == ShaderScalarType::U32);
  static_assert(shader.resource("clipMaskTexture") &&
                shader.resource("clipMaskTexture")->type ==
                    BindingType::SampledTexture2dUnfilterableFloat);
  return true;
}
}  // namespace donner::gpu::shader::programs
