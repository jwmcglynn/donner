/// @file
/// Emits the exact production Slug WGSL and typed solid-fill projection for browser execution.

#include <cstdio>

#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/SlugFill.h"
#include "donner/gpu/shader/programs/SlugGradient.h"
#include "donner/gpu/shader/programs/SlugMask.h"
#include "donner/gpu/shader/programs/SolidFill.h"
#include "donner/gpu/shader/tests/JsonWrite.h"

int main() {
  const auto module = donner::gpu::shader::programs::BuildSolidFillModule();
  if (module.hasError()) {
    std::fputs("Failed to build typed solid-fill module\n", stderr);
    return 1;
  }
  const auto emitted = donner::gpu::shader::EmitWgsl(module.result());
  if (emitted.hasError()) {
    std::fputs("Failed to emit typed solid-fill WGSL\n", stderr);
    return 1;
  }
  const auto& fill = donner::gpu::shader::programs::SlugFillShader().wgsl;
  const auto& gradient = donner::gpu::shader::programs::SlugGradientShader().wgsl;
  const auto& mask = donner::gpu::shader::programs::SlugMaskShader().wgsl;
  if (emitted.result().empty() || fill.empty() || gradient.empty() || mask.empty()) {
    std::fputs("Empty Slug WGSL projection\n", stderr);
    return 1;
  }

  std::fputs("{\"TypedFill\":", stdout);
  donner::gpu::shader::tests::WriteJsonString(emitted.result());
  std::fputs(",\"Fill\":", stdout);
  donner::gpu::shader::tests::WriteJsonString(fill);
  std::fputs(",\"Gradient\":", stdout);
  donner::gpu::shader::tests::WriteJsonString(gradient);
  std::fputs(",\"Mask\":", stdout);
  donner::gpu::shader::tests::WriteJsonString(mask);
  std::fputs("}\n", stdout);
  return std::ferror(stdout) ? 1 : 0;
}
