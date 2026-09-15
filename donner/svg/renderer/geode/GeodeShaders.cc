#include "donner/svg/renderer/geode/GeodeShaders.h"

#include "donner/gpu/shader/programs/ImageBlit.h"
#include "donner/gpu/shader/programs/SlugFill.h"
#include "donner/gpu/shader/programs/SlugGradient.h"
#include "donner/gpu/shader/programs/SlugMask.h"
#include "donner/svg/renderer/geode/GeodeShaderSelection.h"

namespace donner::geode {

gpu::Result<gpu::ShaderModule> createSlugFillShader(gpu::Device& device) {
  return CreateShaderModule(device, gpu::shader::programs::SlugFillShader(),
                            DONNER_GEODE_NATIVE_SHADER(SlugFill), "Slug fill");
}

gpu::Result<gpu::ShaderModule> createSlugGradientShader(gpu::Device& device) {
  return CreateShaderModule(device, gpu::shader::programs::SlugGradientShader(),
                            DONNER_GEODE_NATIVE_SHADER(SlugGradient), "SlugGradient");
}

gpu::Result<gpu::ShaderModule> createSlugMaskShader(gpu::Device& device) {
  return CreateShaderModule(device, gpu::shader::programs::SlugMaskShader(),
                            DONNER_GEODE_NATIVE_SHADER(SlugMask), "SlugMask");
}

gpu::Result<gpu::ShaderModule> createImageBlitShader(gpu::Device& device) {
  return CreateShaderModule(device, gpu::shader::programs::ImageBlitShader(),
                            DONNER_GEODE_NATIVE_SHADER(ImageBlit), "ImageBlit");
}

}  // namespace donner::geode
