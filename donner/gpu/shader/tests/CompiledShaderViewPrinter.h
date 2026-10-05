#pragma once
/// @file
/// Prints a \ref donner::gpu::shader::CompiledShaderView in test failure messages.

#include <ostream>

#include "donner/gpu/shader/CompiledShader.h"

namespace donner::gpu::shader {

/**
 * Prints \p view's projection sizes and the start of its WGSL, so a failed match names the artifact
 * it saw instead of dumping raw bytes.
 *
 * @param view View to print.
 * @param os Output stream.
 */
inline void PrintTo(const CompiledShaderView& view, std::ostream* os) {
  *os << "CompiledShaderView{wgsl " << view.wgsl.size() << " bytes";
  if (!view.wgsl.empty()) {
    *os << " \"" << view.wgsl.substr(0, 40) << (view.wgsl.size() > 40 ? "...\"" : "\"");
  }
  *os << ", msl " << view.msl.size() << " bytes, spirv " << view.spirv.size() << " words, "
      << view.resources.size() << " resources}";
}

}  // namespace donner::gpu::shader
