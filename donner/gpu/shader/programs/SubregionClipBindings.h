#pragma once
/// @file
/// Bind group indices of the subregion-clip compute program.
///
/// Split out from the program header so a host can build the program's bind group layout without
/// linking the shader IR. The pipeline that does so consumes build-time emitted WGSL, and pulling
/// in the IR for an enum would put the emitters back in binaries that exist to avoid them.

#include <cstdint>
#include <string_view>

namespace donner::gpu::shader::programs {

/// Name of the compute entry point, as the IR declares it and the emitted source spells it.
inline constexpr std::string_view kSubregionClipEntryPoint = "cs_main";

/// Workgroup size the entry point declares.
///
/// One definition, read by the IR builder that writes the shader and by every host that creates
/// the pipeline or sizes a dispatch from it. A host restating it could drift, and a larger stale
/// copy would silently under-dispatch, leaving the tail of the destination unwritten.
inline constexpr uint32_t kSubregionClipWorkgroupSize = 8;

/// Bind group indices of the subregion-clip compute program, shared by the IR builder and every
/// host that creates its bind group layout.
enum class SubregionClipBinding : uint32_t {
  InputTexture = 0,   //!< Sampled `texture_2d<f32>` source.
  OutputTexture = 1,  //!< `texture_storage_2d<rgba8unorm, write>` destination.
  Params = 2,         //!< Uniform buffer holding the inverse transform and the user-space
                      //!< rectangle to keep.
};

}  // namespace donner::gpu::shader::programs
