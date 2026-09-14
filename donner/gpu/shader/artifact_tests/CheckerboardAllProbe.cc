#include <cstddef>
#include <cstdint>

#include "donner/gpu/shader/programs/CheckerboardSource.h"

namespace {
constexpr auto kArtifact =
    donner::gpu::shader::wgsl::Compile<donner::gpu::shader::programs::kCheckerboardSource,
                                       donner::gpu::shader::wgsl::Projection::All>();
}

int main() {
  const auto shader = kArtifact.view();
  uint32_t checksum = 0;
  const volatile char* wgsl = shader.wgsl.data();
  const volatile char* msl = shader.msl.data();
  const volatile uint32_t* spirv = shader.spirv.data();
  for (size_t i = 0; i < shader.wgsl.size(); ++i) checksum += static_cast<unsigned char>(wgsl[i]);
  for (size_t i = 0; i < shader.msl.size(); ++i) checksum += static_cast<unsigned char>(msl[i]);
  for (size_t i = 0; i < shader.spirv.size(); ++i) checksum += spirv[i];
  return checksum == 0 ? 1 : 0;
}
