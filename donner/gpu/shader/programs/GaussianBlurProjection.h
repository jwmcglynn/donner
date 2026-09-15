#pragma once
/// @file
/// Compile-time projection policy for the Gaussian blur backend artifacts.

#include "donner/gpu/shader/wgsl/Projection.h"

namespace donner::gpu::shader::programs {

/// Projection consumed by the Geode backend.
inline constexpr wgsl::Projection kGaussianBlurGeodeProjection = wgsl::Projection::Wgsl;

#if !defined(__EMSCRIPTEN__) && (defined(__APPLE__) || defined(__linux__) || defined(_WIN32))
/// Projection consumed by the native backend.
inline constexpr wgsl::Projection kGaussianBlurNativeProjection = wgsl::kNativeProjection;
#endif

}  // namespace donner::gpu::shader::programs
