#include "donner/gpu/shader/programs/RoundHalfAwayFromZero.h"

#include <utility>
#include <vector>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"

namespace donner::gpu::shader::programs {

ShaderStatus AddRoundHalfAwayFromZero(ModuleBuilder& builder) {
  ErrorLatch e;

  ShaderResult<FunctionBuilder> rounder = builder.createFunction(
      RcString(kRoundHalfAwayFromZeroName), {IrParam{"x", IrType::F32()}}, IrType::F32());
  if (rounder.hasError()) {
    return std::move(rounder).error();
  }
  FunctionBuilder fn = std::move(rounder).result();

  const IrExpr x = e(fn.ref("x"));
  const IrExpr magnitude = e(CallBuiltin(BuiltinFn::Abs, {x}));
  const IrExpr shifted = e(Add(magnitude, LiteralF32(0.5f)));
  const IrExpr truncated = e(CallBuiltin(BuiltinFn::Floor, {shifted}));
  e.ok(fn.returnValue(e(Mul(e(CallBuiltin(BuiltinFn::Sign, {x})), truncated))));
  e.ok(fn.finish());

  if (e.error) {
    return *e.error;
  }
  return OkShaderStatus();
}

}  // namespace donner::gpu::shader::programs
