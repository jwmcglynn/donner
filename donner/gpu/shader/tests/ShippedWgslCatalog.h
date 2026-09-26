#pragma once
/// @file
/// Test-only census of the WGSL projections linked into production artifacts.

#include <span>

#include "donner/gpu/shader/CompiledShader.h"

namespace donner::gpu::shader::tests {

/// A shipped artifact and its process-lifetime WGSL projection.
struct ShippedWgslCase {
  const char* artifactTarget;             //!< Bazel target name for the production WGSL artifact.
  const CompiledShaderView& (*shader)();  //!< Returns the frozen artifact.
};

/// Returns a span over process-lifetime entries; callers never own or mutate it.
std::span<const ShippedWgslCase> ShippedWgslCatalog();

}  // namespace donner::gpu::shader::tests
