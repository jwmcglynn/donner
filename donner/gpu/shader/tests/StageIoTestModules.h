#pragma once
/// @file
/// Modules built to hold the emitters to stage IO shapes no shipped program has.
///
/// Kept out of ShaderTestUtils.h on purpose: this header pulls in the IR, whose expression
/// factories include names like `Eq` that collide with the gmock matchers those test files bring
/// into the same namespace.

#include <utility>

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/ShaderResult.h"

namespace donner::gpu::shader {

/**
 * A fragment entry point whose only input is the position builtin, so it declares no location at
 * all.
 *
 * Each emitter decides separately how a location-less builtin input reaches the stage, which
 * makes this the shape an emitter can get wrong without any program noticing. All three are held
 * to it.
 *
 * @return The built module, or the first construction error.
 */
inline ShaderResult<IrModule> BuildPositionOnlyFragmentModule() {
  ModuleBuilder builder;
  ShaderResult<FunctionBuilder> entry = builder.createFragmentEntryPoint(
      "fs_position_only",
      {IrParam{"frag_pos", IrType::Vec4f(), std::nullopt, BuiltinInput::Position}},
      {IrOutputMember{"color", IrType::Vec4f(), 0}});
  if (entry.hasError()) {
    return std::move(entry).error();
  }
  FunctionBuilder fn = std::move(entry).result();

  ShaderResult<IrExpr> fragPos = fn.ref("frag_pos");
  if (fragPos.hasError()) {
    return std::move(fragPos).error();
  }
  if (ShaderStatus status = fn.returnOutputs({fragPos.result()}); status.hasError()) {
    return std::move(status).error();
  }
  if (ShaderStatus status = fn.finish(); status.hasError()) {
    return std::move(status).error();
  }
  return builder.build();
}

}  // namespace donner::gpu::shader
