#pragma once
/// @file
/// feComposite parameters, operators and precompiled shader projections.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Compositing operators as the shader decodes them.
enum class CompositeOperator : uint32_t {
  Over = 0,        //!< Source over destination.
  In = 1,          //!< Source inside destination.
  Out = 2,         //!< Source outside destination.
  Atop = 3,        //!< Source atop destination.
  Xor = 4,         //!< Nonoverlapping source and destination.
  Lighter = 5,     //!< Saturating sum.
  Arithmetic = 6,  //!< Weighted product, sum, and constant.
};
/// Uniform operator selection and arithmetic coefficients.
struct CompositeParams {
  uint32_t op;    //!< \ref CompositeOperator value.
  uint32_t pad0;  //!< Reserved layout padding.
  uint32_t pad1;  //!< Reserved layout padding.
  uint32_t pad2;  //!< Reserved layout padding.
  float k1;       //!< Arithmetic product coefficient.
  float k2;       //!< Arithmetic source coefficient.
  float k3;       //!< Arithmetic destination coefficient.
  float k4;       //!< Arithmetic constant term.
};
static_assert(sizeof(CompositeParams) == 32);
/// Returns the WGSL composite artifact for the six Porter-Duff operators and arithmetic.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& CompositeShader();
/// Returns only the platform-native Composite projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& CompositeNativeShader();
}  // namespace donner::gpu::shader::programs
