/// @file
/// Emits the exact production WGSL projections for an independent browser shader compiler.

#include <cstdio>

#include "donner/gpu/shader/tests/JsonWrite.h"
#include "donner/gpu/shader/tests/ShippedWgslCatalog.h"

int main() {
  using donner::gpu::shader::tests::ShippedWgslCatalog;
  std::fputs("[\n", stdout);
  bool first = true;
  for (const auto& entry : ShippedWgslCatalog()) {
    const donner::gpu::shader::CompiledShaderView& shader = entry.shader();
    if (shader.wgsl.empty() || !shader.msl.empty() || !shader.spirv.empty()) {
      std::fprintf(stderr, "Invalid WGSL-only production artifact: %s\n", entry.artifactTarget);
      return 1;
    }
    if (!first) {
      std::fputs(",\n", stdout);
    }
    first = false;
    std::fputs("  {\"artifact\":", stdout);
    donner::gpu::shader::tests::WriteJsonString(entry.artifactTarget);
    std::fputs(",\"wgsl\":", stdout);
    donner::gpu::shader::tests::WriteJsonString(shader.wgsl);
    std::putchar('}');
  }
  std::fputs("\n]\n", stdout);
  return std::ferror(stdout) ? 1 : 0;
}
