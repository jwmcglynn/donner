#pragma once
/// @file
/// Explicit output selection for shader compilation.

#include <cstdint>

namespace donner::gpu::shader::wgsl {

/// Projections materialized in the final application artifact.
enum class Projection : uint8_t { Wgsl = 1, Msl = 2, Spirv = 4, All = 7 };

#if !defined(__EMSCRIPTEN__) && defined(__APPLE__)
/// Projection used by the native device on this platform.
inline constexpr Projection kNativeProjection = Projection::Msl;
#elif !defined(__EMSCRIPTEN__) && (defined(__linux__) || defined(_WIN32))
/// Projection used by the native device on this platform.
inline constexpr Projection kNativeProjection = Projection::Spirv;
#endif

}  // namespace donner::gpu::shader::wgsl
