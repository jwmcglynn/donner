/// @file
/// Emits test-only WGSL and half-boundary inputs for independent browser execution.

#include <cstdio>

#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/tests/JsonWrite.h"
#include "donner/gpu/shader/tests/MathPrimitiveCoverageModule.h"

int main() {
  const auto module = donner::gpu::shader::BuildMathPrimitiveModule();
  if (module.hasError()) {
    std::fputs("Failed to build the math primitive module\n", stderr);
    return 1;
  }
  const auto emitted = donner::gpu::shader::EmitWgsl(module.result());
  if (emitted.hasError()) {
    std::fputs("Failed to emit the math primitive WGSL\n", stderr);
    return 1;
  }

  std::fputs("{\"wgsl\":", stdout);
  donner::gpu::shader::tests::WriteJsonString(emitted.result());
  std::fputs(",\"values\":[", stdout);
  bool first = true;
  for (float value : donner::gpu::shader::MathPrimitiveInputValues()) {
    if (!first) {
      std::putchar(',');
    }
    first = false;
    std::fprintf(stdout, "%.9g", static_cast<double>(value));
  }
  std::fputs("]}\n", stdout);
  return std::ferror(stdout) ? 1 : 0;
}
