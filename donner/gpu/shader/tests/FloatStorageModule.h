#pragma once
/// @file
/// Float storage module shared by shader front-end validation tests.

#include <utility>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"

namespace donner::gpu::shader {

/// Builds a compute program that preserves an input float texel and adds 1/4096 per channel.
inline ShaderResult<IrModule> BuildFloatStorageModule() {
  programs::ErrorLatch e;
  ModuleBuilder builder;
  e.ok(builder.addTexture2d(0, 0, "input"));
  e.ok(builder.addWriteOnlyStorageTexture2d(0, 1, "output", StorageTextureFormat::Rgba32Float));
  auto entry = builder.createComputeEntryPoint("cs_main", {}, WorkgroupSize{1, 1, 1});
  if (entry.hasError()) {
    return std::move(entry).error();
  }
  FunctionBuilder fn = std::move(entry).result();
  const IrExpr coords = e(ConstructVector(IrType::Vec2i(), {LiteralI32(0), LiteralI32(0)}));
  const IrExpr input =
      e(CallBuiltin(BuiltinFn::TextureLoad, {e(fn.ref("input")), coords, LiteralI32(0)}));
  const IrExpr increment = e(ConstructVector(IrType::Vec4f(), {LiteralF32(1.0f / 4096.0f)}));
  const IrExpr color = e(Add(input, increment));
  e.ok(fn.textureStore(e(fn.ref("output")), coords, color));
  e.ok(fn.finish());
  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

}  // namespace donner::gpu::shader
