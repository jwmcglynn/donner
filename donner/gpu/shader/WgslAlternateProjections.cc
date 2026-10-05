/// @file
/// Registers the authored WGSL of every production shader family as the alternate of the family's
/// linked native artifact. A native build links only the native projections, so a test that drives
/// production pipelines through a device consuming WGSL (a recording device, a test device that
/// captures descriptors, or the Linux WebGPU reference) links this test-only library to supply
/// them.

#include "donner/gpu/shader/LinkedProjectionTesting.h"
#include "donner/gpu/shader/ProductionShaderFamilies.h"

namespace donner::gpu::shader {
namespace {

/// Registers every production family's authored WGSL for its native artifact.
/// @return True, so the registration can initialize a namespace constant.
bool RegisterWgslAlternates() {
#define DONNER_REGISTER_WGSL_ALTERNATE(family)                                          \
  RegisterAlternateProjection(programs::family##NativeShader(), ShaderSourceKind::Wgsl, \
                              programs::family##Shader());
  DONNER_FOR_EACH_PRODUCTION_SHADER_FAMILY(DONNER_REGISTER_WGSL_ALTERNATE)
#undef DONNER_REGISTER_WGSL_ALTERNATE
  return true;
}

/// Runs the registration during static initialization, before any device exists.
[[maybe_unused]] const bool kWgslAlternatesRegistered = RegisterWgslAlternates();

}  // namespace
}  // namespace donner::gpu::shader
