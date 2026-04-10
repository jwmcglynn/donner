#include "donner/svg/renderer/geode/GeodeShaders.h"

#include "embed_resources/ImageBlitWgsl.h"
#include "embed_resources/SlugFillWgsl.h"
#include "embed_resources/SlugGradientWgsl.h"

namespace donner::geode {

wgpu::ShaderModule createSlugFillShader(const wgpu::Device& device) {
  // The WGSL source is embedded at build time from shaders/slug_fill.wgsl
  // (see :slug_fill_wgsl rule in BUILD.bazel). The embedded resource has no
  // NUL terminator, so we use the explicit-length form of `ShaderSourceWGSL`.
  wgpu::ShaderSourceWGSL wgslSource = {};
  wgslSource.code.data =
      reinterpret_cast<const char*>(donner::embedded::kSlugFillWgsl.data());
  wgslSource.code.length = donner::embedded::kSlugFillWgsl.size();

  wgpu::ShaderModuleDescriptor desc = {};
  desc.label = "SlugFill";
  desc.nextInChain = &wgslSource;

  return device.CreateShaderModule(&desc);
}

wgpu::ShaderModule createSlugGradientShader(const wgpu::Device& device) {
  // The WGSL source is embedded at build time from shaders/slug_gradient.wgsl
  // (see :slug_gradient_wgsl rule in BUILD.bazel).
  wgpu::ShaderSourceWGSL wgslSource = {};
  wgslSource.code.data =
      reinterpret_cast<const char*>(donner::embedded::kSlugGradientWgsl.data());
  wgslSource.code.length = donner::embedded::kSlugGradientWgsl.size();

  wgpu::ShaderModuleDescriptor desc = {};
  desc.label = "SlugGradient";
  desc.nextInChain = &wgslSource;

  return device.CreateShaderModule(&desc);
}

wgpu::ShaderModule createImageBlitShader(const wgpu::Device& device) {
  // Embedded at build time from shaders/image_blit.wgsl via the
  // :image_blit_wgsl rule in BUILD.bazel.
  wgpu::ShaderSourceWGSL wgslSource = {};
  wgslSource.code.data =
      reinterpret_cast<const char*>(donner::embedded::kImageBlitWgsl.data());
  wgslSource.code.length = donner::embedded::kImageBlitWgsl.size();

  wgpu::ShaderModuleDescriptor desc = {};
  desc.label = "ImageBlit";
  desc.nextInChain = &wgslSource;

  return device.CreateShaderModule(&desc);
}

}  // namespace donner::geode
