#include "donner/gpu/shader/programs/GaussianBlur.h"

#include <cstddef>

#include "donner/gpu/shader/programs/GaussianBlurSource.h"

namespace donner::gpu::shader::programs {
namespace {

#if defined(__EMSCRIPTEN__)
constexpr auto kGaussianBlurArtifact = wgsl::Compile<kGaussianBlurSource, wgsl::Projection::Wgsl>();
#elif defined(__APPLE__)
constexpr auto kGaussianBlurArtifact =
    wgsl::Compile<kGaussianBlurSource,
                  static_cast<wgsl::Projection>(static_cast<uint8_t>(wgsl::Projection::Wgsl) |
                                                static_cast<uint8_t>(wgsl::Projection::Msl))>();
#elif defined(__linux__) || defined(_WIN32)
constexpr auto kGaussianBlurArtifact =
    wgsl::Compile<kGaussianBlurSource,
                  static_cast<wgsl::Projection>(static_cast<uint8_t>(wgsl::Projection::Wgsl) |
                                                static_cast<uint8_t>(wgsl::Projection::Spirv))>();
#else
constexpr auto kGaussianBlurArtifact = wgsl::Compile<kGaussianBlurSource>();
#endif

constexpr CompiledShaderView kGaussianBlurView = kGaussianBlurArtifact.view();
constexpr const ShaderResource* kParamsResource = kGaussianBlurView.resource("params");
constexpr const ShaderResource* kInputResource = kGaussianBlurView.resource("inputTexture");
constexpr const ShaderResource* kOutputResource = kGaussianBlurView.resource("outputTexture");

static_assert(kInputResource &&
              kInputResource->type == BindingType::SampledTexture2dUnfilterableFloat);
static_assert(kOutputResource && kOutputResource->type == BindingType::WriteOnlyStorageTexture2d);
static_assert(kGaussianBlurView.workgroupSize[2] == 1, "Gaussian blur dispatch is two-dimensional");
static_assert(kParamsResource != nullptr);
static_assert(kParamsResource->type == BindingType::UniformBuffer);
static_assert(kParamsResource->minSizeBytes == sizeof(GaussianBlurParams));
static_assert(kParamsResource->alignmentBytes == alignof(GaussianBlurParams));
static_assert(kGaussianBlurView.matchesMember("params", "stdDeviation",
                                              offsetof(GaussianBlurParams, stdDeviation),
                                              sizeof(GaussianBlurParams::stdDeviation),
                                              ShaderScalarType::F32));
static_assert(kGaussianBlurView.matchesMember("params", "axis", offsetof(GaussianBlurParams, axis),
                                              sizeof(GaussianBlurParams::axis),
                                              ShaderScalarType::U32));
static_assert(kGaussianBlurView.matchesMember("params", "edgeMode",
                                              offsetof(GaussianBlurParams, edgeMode),
                                              sizeof(GaussianBlurParams::edgeMode),
                                              ShaderScalarType::U32));
static_assert(kGaussianBlurView.matchesMember("params", "kernelType",
                                              offsetof(GaussianBlurParams, kernelType),
                                              sizeof(GaussianBlurParams::kernelType),
                                              ShaderScalarType::U32));
static_assert(kGaussianBlurView.matchesMember("params", "boxLeft",
                                              offsetof(GaussianBlurParams, boxLeft),
                                              sizeof(GaussianBlurParams::boxLeft),
                                              ShaderScalarType::I32));
static_assert(kGaussianBlurView.matchesMember("params", "boxRight",
                                              offsetof(GaussianBlurParams, boxRight),
                                              sizeof(GaussianBlurParams::boxRight),
                                              ShaderScalarType::I32));
static_assert(kGaussianBlurView.matchesMember("params", "clipMin",
                                              offsetof(GaussianBlurParams, clipMin),
                                              sizeof(GaussianBlurParams::clipMin),
                                              ShaderScalarType::I32, 2));
static_assert(kGaussianBlurView.matchesMember("params", "clipMax",
                                              offsetof(GaussianBlurParams, clipMax),
                                              sizeof(GaussianBlurParams::clipMax),
                                              ShaderScalarType::I32, 2));
static_assert(kGaussianBlurView.matchesMember("params", "clipActive",
                                              offsetof(GaussianBlurParams, clipActive),
                                              sizeof(GaussianBlurParams::clipActive),
                                              ShaderScalarType::U32));
static_assert(kGaussianBlurView.matchesMember("params", "pad", offsetof(GaussianBlurParams, pad),
                                              sizeof(GaussianBlurParams::pad),
                                              ShaderScalarType::U32));

}  // namespace

const CompiledShaderView& GaussianBlurShader() {
  return kGaussianBlurView;
}

}  // namespace donner::gpu::shader::programs
