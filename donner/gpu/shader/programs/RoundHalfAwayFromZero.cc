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
  const IrExpr magnitude = e(fn.addLet("magnitude", e(CallBuiltin(BuiltinFn::Abs, {x}))));
  const IrExpr integral = e(fn.addLet("integral", e(CallBuiltin(BuiltinFn::Floor, {magnitude}))));
  const IrExpr sign = e(CallBuiltin(BuiltinFn::Sign, {x}));
  // Adding 0.5 first can round an input immediately below a half to the next integer.
  e.ok(fn.beginIf(e(Ge(e(Sub(magnitude, integral)), LiteralF32(0.5f)))));
  e.ok(fn.returnValue(e(Mul(sign, e(Add(integral, LiteralF32(1.0f)))))));
  e.ok(fn.endIf());
  e.ok(fn.returnValue(e(Mul(sign, integral))));
  e.ok(fn.finish());

  if (e.error) {
    return *e.error;
  }
  return OkShaderStatus();
}

}  // namespace donner::gpu::shader::programs
