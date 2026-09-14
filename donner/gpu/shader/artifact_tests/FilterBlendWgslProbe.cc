#include <cstddef>
#include <cstdint>

#include "donner/gpu/shader/programs/FilterBlend.h"

int main() {
  const auto& shader = donner::gpu::shader::programs::FilterBlendShader();
  uint32_t checksum = 0;
  const volatile char* wgsl = shader.wgsl.data();
  const volatile char* msl = shader.msl.data();
  const volatile uint32_t* spirv = shader.spirv.data();
  for (size_t i = 0; i < shader.wgsl.size(); ++i) checksum += static_cast<unsigned char>(wgsl[i]);
  for (size_t i = 0; i < shader.msl.size(); ++i) checksum += static_cast<unsigned char>(msl[i]);
  for (size_t i = 0; i < shader.spirv.size(); ++i) checksum += spirv[i];
  return checksum == 0 ? 1 : 0;
}
