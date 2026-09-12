#pragma once
/// @file
/// Transparency checkerboard render program with device-pixel anchoring.

#include "donner/gpu/shader/IrModule.h"

namespace donner::gpu::shader::programs {

/// Builds vs_main and fs_main. Group 0 binding 0 is the 64-byte checkerboard
/// uniform block.
ShaderResult<IrModule> BuildCheckerboardModule();

}  // namespace donner::gpu::shader::programs
