#pragma once
/// @file
/// Compile-time image-blit resource, interface and uniform verification.
#include <cstddef>

#include "donner/gpu/shader/programs/ImageBlit.h"
namespace donner::gpu::shader::programs {
/// Checks every admitted uniform field and resource while permitting reflected binding changes.
/// @tparam shader Static reflected shader interface.
template <const CompiledShaderView& shader>
consteval bool ValidateImageBlitArtifact() {
  static_assert(shader.entryPoints.size() == 2);
  static_assert(shader.entryPoints[0].stage == ShaderStage::Vertex);
  static_assert(shader.entryPoints[1].stage == ShaderStage::Fragment);
  static_assert(shader.resources.size() == 6);
  constexpr auto* uniforms = shader.resource("uniforms");
  static_assert(uniforms && uniforms->type == BindingType::UniformBuffer);
  static_assert(uniforms->minSizeBytes == sizeof(ImageBlitParams));
  static_assert(uniforms->alignmentBytes == alignof(ImageBlitParams));
  static_assert(shader.resource("imageSampler") &&
                shader.resource("imageSampler")->type == BindingType::FilteringSampler);
  static_assert(shader.resource("imageTexture") &&
                shader.resource("imageTexture")->type == BindingType::SampledTexture2dFloat);
  static_assert(shader.resource("maskTexture") &&
                shader.resource("maskTexture")->type == BindingType::SampledTexture2dFloat);
  static_assert(shader.resource("dstSnapshotTexture") &&
                shader.resource("dstSnapshotTexture")->type == BindingType::SampledTexture2dFloat);
  static_assert(shader.resource("clipMaskTexture") &&
                shader.resource("clipMaskTexture")->type ==
                    BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(shader.matchesMember("uniforms", "mvp", offsetof(ImageBlitParams, mvp),
                                     sizeof(ImageBlitParams::mvp), ShaderScalarType::F32, 4, 0, 0,
                                     4, 16));
  static_assert(shader.matchesMember("uniforms", "destRect", offsetof(ImageBlitParams, destRect),
                                     sizeof(ImageBlitParams::destRect), ShaderScalarType::F32, 4, 0,
                                     0, 0, 0));
  static_assert(shader.matchesMember("uniforms", "srcRect", offsetof(ImageBlitParams, srcRect),
                                     sizeof(ImageBlitParams::srcRect), ShaderScalarType::F32, 4, 0,
                                     0, 0, 0));
  static_assert(shader.matchesMember(
      "uniforms", "targetSize", offsetof(ImageBlitParams, targetSize),
      sizeof(ImageBlitParams::targetSize), ShaderScalarType::F32, 2, 0, 0, 0, 0));
  static_assert(shader.matchesMember("uniforms", "opacity", offsetof(ImageBlitParams, opacity),
                                     sizeof(ImageBlitParams::opacity), ShaderScalarType::F32, 1, 0,
                                     0, 0, 0));
  static_assert(shader.matchesMember(
      "uniforms", "sourceIsPremult", offsetof(ImageBlitParams, sourceIsPremult),
      sizeof(ImageBlitParams::sourceIsPremult), ShaderScalarType::U32, 1, 0, 0, 0, 0));
  static_assert(shader.matchesMember("uniforms", "maskMode", offsetof(ImageBlitParams, maskMode),
                                     sizeof(ImageBlitParams::maskMode), ShaderScalarType::U32, 1, 0,
                                     0, 0, 0));
  static_assert(shader.matchesMember(
      "uniforms", "applyMaskBounds", offsetof(ImageBlitParams, applyMaskBounds),
      sizeof(ImageBlitParams::applyMaskBounds), ShaderScalarType::U32, 1, 0, 0, 0, 0));
  static_assert(shader.matchesMember(
      "uniforms", "maskBounds", offsetof(ImageBlitParams, maskBounds),
      sizeof(ImageBlitParams::maskBounds), ShaderScalarType::F32, 4, 0, 0, 0, 0));
  static_assert(shader.matchesMember("uniforms", "blendMode", offsetof(ImageBlitParams, blendMode),
                                     sizeof(ImageBlitParams::blendMode), ShaderScalarType::U32, 1,
                                     0, 0, 0, 0));
  static_assert(shader.matchesMember(
      "uniforms", "hasClipMask", offsetof(ImageBlitParams, hasClipMask),
      sizeof(ImageBlitParams::hasClipMask), ShaderScalarType::U32, 1, 0, 0, 0, 0));
  static_assert(shader.matchesMember(
      "uniforms", "samplingMode", offsetof(ImageBlitParams, samplingMode),
      sizeof(ImageBlitParams::samplingMode), ShaderScalarType::U32, 1, 0, 0, 0, 0));
  static_assert(shader.matchesMember(
      "uniforms", "_blendPad1", offsetof(ImageBlitParams, blendPadding),
      sizeof(ImageBlitParams::blendPadding), ShaderScalarType::U32, 1, 0, 0, 0, 0));
  static_assert(shader.matchesMember(
      "uniforms", "pixelatedScale", offsetof(ImageBlitParams, pixelatedScale),
      sizeof(ImageBlitParams::pixelatedScale), ShaderScalarType::F32, 2, 0, 0, 0, 0));
  static_assert(shader.matchesMember(
      "uniforms", "_samplingPad", offsetof(ImageBlitParams, samplingPadding),
      sizeof(ImageBlitParams::samplingPadding), ShaderScalarType::U32, 2, 0, 0, 0, 0));
  return true;
}
}  // namespace donner::gpu::shader::programs
