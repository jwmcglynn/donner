#include "donner/svg/renderer/geode/GeodeShaders.h"

#include "donner/gpu/shader/programs/ImageBlit.h"
#include "donner/gpu/shader/programs/SlugFill.h"
#include "donner/gpu/shader/programs/SlugGradient.h"
#include "donner/gpu/shader/programs/SlugMask.h"

namespace donner::geode {

gpu::Result<gpu::ShaderModule> createSlugFillShader(gpu::Device& device) {
  return device.createShaderModule(gpu::shader::MakeShaderDescriptor(
      gpu::shader::programs::SlugFillShader(), gpu::ShaderSourceKind::Wgsl, "Slug fill"));
}

gpu::Result<gpu::ShaderModule> createSlugGradientShader(gpu::Device& device) {
  return device.createShaderModule(gpu::shader::MakeShaderDescriptor(
      gpu::shader::programs::SlugGradientShader(), gpu::ShaderSourceKind::Wgsl, "SlugGradient"));
}

gpu::Result<gpu::ShaderModule> createSlugMaskShader(gpu::Device& device) {
  return device.createShaderModule(gpu::shader::MakeShaderDescriptor(
      gpu::shader::programs::SlugMaskShader(), gpu::ShaderSourceKind::Wgsl, "SlugMask"));
}

gpu::Result<gpu::ShaderModule> createImageBlitShader(gpu::Device& device) {
  return device.createShaderModule(gpu::shader::MakeShaderDescriptor(
      gpu::shader::programs::ImageBlitShader(), gpu::ShaderSourceKind::Wgsl, "ImageBlit"));
}

}  // namespace donner::geode
