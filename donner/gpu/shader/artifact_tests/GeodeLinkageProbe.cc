// Links what the production libraries link: the one artifact per family a native build names with
// DONNER_LINKED_SHADER_ARTIFACT, for every family they reach. The projection inspector then
// confirms the linked binary carries the platform-native payload and no authored WGSL.

#include <cstddef>
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/LinkedProjection.h"
#include "donner/gpu/shader/ProductionShaderFamilies.h"

namespace {

/// Reads every byte of each projection so the linker keeps the whole artifact.
/// @param shader Frozen artifact to read.
uint32_t Checksum(const donner::gpu::shader::CompiledShaderView& shader) {
  uint32_t checksum = 0;
  const volatile char* wgsl = shader.wgsl.data();
  const volatile char* msl = shader.msl.data();
  const volatile uint32_t* spirv = shader.spirv.data();
  for (size_t i = 0; i < shader.wgsl.size(); ++i) {
    checksum += static_cast<unsigned char>(wgsl[i]);
  }
  for (size_t i = 0; i < shader.msl.size(); ++i) {
    checksum += static_cast<unsigned char>(msl[i]);
  }
  for (size_t i = 0; i < shader.spirv.size(); ++i) {
    checksum += spirv[i];
  }
  return checksum;
}

}  // namespace

int main() {
#define DONNER_LINKED_ARTIFACT_ADDRESS(family) &DONNER_LINKED_SHADER_ARTIFACT(family),
  const donner::gpu::shader::CompiledShaderView* const shaders[] = {
      DONNER_FOR_EACH_PRODUCTION_SHADER_FAMILY(DONNER_LINKED_ARTIFACT_ADDRESS)};
#undef DONNER_LINKED_ARTIFACT_ADDRESS
  uint32_t checksum = 0;
  for (const donner::gpu::shader::CompiledShaderView* shader : shaders) {
    checksum += Checksum(*shader);
  }
  return checksum == 0 ? 1 : 0;
}
