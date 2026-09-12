#include "donner/gpu/shader/programs/Checkerboard.h"

#include <optional>
#include <utility>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"

namespace donner::gpu::shader::programs {
namespace {

ShaderStatus AddVertex(ModuleBuilder& builder, ErrorLatch& e) {
  auto entry = builder.createVertexEntryPoint(
      "vs_main", {IrParam{"vertex_index", IrType::U32(), std::nullopt, BuiltinInput::VertexIndex}},
      {IrOutputMember{"position", IrType::Vec4f(), std::nullopt, BuiltinOutput::Position}});
  if (entry.hasError()) return std::move(entry).error();
  FunctionBuilder fn = std::move(entry).result();
  const IrExpr index = e(fn.ref("vertex_index"));
  // The fullscreen triangle needs no vertex buffer.
  const IrExpr x = e(
      CallBuiltin(BuiltinFn::Select, {LiteralF32(-1), LiteralF32(3), e(Eq(index, LiteralU32(1)))}));
  const IrExpr y = e(
      CallBuiltin(BuiltinFn::Select, {LiteralF32(-1), LiteralF32(3), e(Eq(index, LiteralU32(2)))}));
  e.ok(fn.returnOutputs(
      {e(ConstructVector(IrType::Vec4f(), {x, y, LiteralF32(0), LiteralF32(1)}))}));
  return fn.finish();
}

ShaderStatus AddFragment(ModuleBuilder& builder, ErrorLatch& e) {
  auto entry = builder.createFragmentEntryPoint(
      "fs_main", {IrParam{"position", IrType::Vec4f(), std::nullopt, BuiltinInput::Position}},
      {IrOutputMember{"color", IrType::Vec4f(), 0}});
  if (entry.hasError()) return std::move(entry).error();
  FunctionBuilder fn = std::move(entry).result();
  const IrExpr params = e(fn.ref("params"));
  const IrExpr position = e(fn.ref("position"));
  const IrExpr anchored = e(fn.addLet(
      "anchored", e(Add(e(CallBuiltin(BuiltinFn::Min, {e(Swizzle(position, "xy")),
                                                       e(Member(params, "target_size"))})),
                        e(Member(params, "origin_offset"))))));
  const IrExpr screen = e(fn.addLet(
      "screen",
      e(Div(anchored, e(CallBuiltin(BuiltinFn::Max, {e(Member(params, "device_pixel_ratio")),
                                                     LiteralF32(0.0001f)}))))));
  const IrExpr size = e(Member(params, "checker_size"));
  const IrExpr cell = e(fn.addLet(
      "cell",
      e(Convert(
          IrType::Vec2i(),
          e(CallBuiltin(BuiltinFn::Floor,
                        {e(Div(screen, e(ConstructVector(IrType::Vec2f(), {size, size}))))}))))));
  const IrExpr sum = e(Add(e(Swizzle(cell, "x")), e(Swizzle(cell, "y"))));
  // Comparing the remainder to zero preserves parity for negative cell indices.
  e.ok(fn.beginIf(e(Eq(e(Mod(sum, LiteralI32(2))), LiteralI32(0)))));
  e.ok(fn.returnOutputs({e(Member(params, "light_color"))}));
  e.ok(fn.endIf());
  e.ok(fn.returnOutputs({e(Member(params, "dark_color"))}));
  return fn.finish();
}

}  // namespace

ShaderResult<IrModule> BuildCheckerboardModule() {
  ModuleBuilder builder;
  ErrorLatch e;
  const IrType params =
      e(IrType::Struct("CheckerboardParams", {{"target_size", IrType::Vec2f()},
                                              {"device_pixel_ratio", IrType::F32()},
                                              {"checker_size", IrType::F32()},
                                              {"dark_color", IrType::Vec4f()},
                                              {"light_color", IrType::Vec4f()},
                                              {"origin_offset", IrType::Vec2f()},
                                              {"padding", IrType::Vec2f()}}));
  e.ok(builder.addUniformBuffer(0, 0, "params", params));
  e.ok(AddVertex(builder, e));
  e.ok(AddFragment(builder, e));
  if (e.error) return *e.error;
  return builder.build();
}

}  // namespace donner::gpu::shader::programs
