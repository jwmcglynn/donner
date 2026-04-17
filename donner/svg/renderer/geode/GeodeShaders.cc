#include "donner/svg/renderer/geode/GeodeShaders.h"

#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#include "embed_resources/ImageBlitWgsl.h"
#include "embed_resources/SlugFillWgsl.h"
#include "embed_resources/SlugFillAlphaCoverageWgsl.h"
#include "embed_resources/SlugGradientWgsl.h"
#include "embed_resources/SlugGradientAlphaCoverageWgsl.h"
#include "embed_resources/SlugMaskWgsl.h"
#include "embed_resources/SlugMaskAlphaCoverageWgsl.h"

namespace donner::geode {

namespace {

/// Build a `wgpu::ShaderModule` from a raw byte buffer of WGSL source.
/// The embedded WGSL blobs in `//donner/svg/renderer/geode:*_wgsl` land
/// here as `std::span<const unsigned char>` — wgpu-native's shader-
/// module descriptor wants a pointer + length as a `WGPUStringView`
/// (since the newer WebGPU headers use string views everywhere instead
/// of NUL-terminated C strings).
///
/// `wgpu::ShaderSourceWGSL{wgpu::Default}` runs the generated
/// `setDefault()` which fills in `chain.sType = SType::ShaderSourceWGSL`
/// so the downstream `nextInChain = &source.chain` pointer walks the
/// right struct type.
wgpu::ShaderModule createShaderFromWgsl(const wgpu::Device& device,
                                         const char* label,
                                         const unsigned char* source,
                                         size_t sourceSize) {
  wgpu::ShaderSourceWGSL wgslSource{wgpu::Default};
  wgslSource.code.data = reinterpret_cast<const char*>(source);
  wgslSource.code.length = sourceSize;

  wgpu::ShaderModuleDescriptor desc{wgpu::Default};
  desc.label = wgpuLabel(label);
  desc.nextInChain = &wgslSource.chain;

  return device.createShaderModule(desc);
}

}  // namespace

wgpu::ShaderModule createSlugFillShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "SlugFill",
                              donner::embedded::kSlugFillWgsl.data(),
                              donner::embedded::kSlugFillWgsl.size());
}

wgpu::ShaderModule createSlugGradientShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "SlugGradient",
                              donner::embedded::kSlugGradientWgsl.data(),
                              donner::embedded::kSlugGradientWgsl.size());
}

wgpu::ShaderModule createSlugMaskShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "SlugMask",
                              donner::embedded::kSlugMaskWgsl.data(),
                              donner::embedded::kSlugMaskWgsl.size());
}

wgpu::ShaderModule createImageBlitShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "ImageBlit",
                              donner::embedded::kImageBlitWgsl.data(),
                              donner::embedded::kImageBlitWgsl.size());
}

wgpu::ShaderModule createSlugFillAlphaCoverageShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "SlugFillAlphaCoverage",
                              donner::embedded::kSlugFillAlphaCoverageWgsl.data(),
                              donner::embedded::kSlugFillAlphaCoverageWgsl.size());
}

wgpu::ShaderModule createSlugGradientAlphaCoverageShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "SlugGradientAlphaCoverage",
                              donner::embedded::kSlugGradientAlphaCoverageWgsl.data(),
                              donner::embedded::kSlugGradientAlphaCoverageWgsl.size());
}

wgpu::ShaderModule createSlugMaskAlphaCoverageShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "SlugMaskAlphaCoverage",
                              donner::embedded::kSlugMaskAlphaCoverageWgsl.data(),
                              donner::embedded::kSlugMaskAlphaCoverageWgsl.size());
}

}  // namespace donner::geode
