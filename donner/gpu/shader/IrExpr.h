#pragma once
/// @file
/// Typed, immutable expression nodes for the \c donner::gpu::shader IR.
///
/// Every expression is type-checked at construction: the factory functions return
/// \ref ShaderResult and fail closed on ill-typed operands, unknown swizzles, bad constructor
/// arity, indexing of non-indexable types, and unknown builtin functions. Expression values are
/// cheap immutable handles; sharing a subexpression is sharing, not mutation.

#include <cstdint>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/gpu/shader/IrType.h"
#include "donner/gpu/shader/ShaderResult.h"

namespace donner::gpu::shader {

/// Binary operators.
enum class BinaryOp : uint8_t {
  Add,  //!< `+`
  Sub,  //!< `-`
  Mul,  //!< `*` (scalars, vectors, vector*scalar, mat4x4f*vec4f, mat4x4f*mat4x4f)
  Div,  //!< `/`
  Lt,   //!< `<`
  Le,   //!< `<=`
  Gt,   //!< `>`
  Ge,   //!< `>=`
  Eq,   //!< `==`
  Ne,   //!< `!=`
  And,  //!< `&&`
  Or,   //!< `||`
};

/// Ostream output operator, e.g. `add`. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, BinaryOp value);

/// Builtin functions callable from IR (the solid-fill subset; everything else is rejected).
enum class BuiltinFn : uint8_t {
  Abs,                //!< `abs(x)`
  Min,                //!< `min(a, b)`
  Max,                //!< `max(a, b)`
  Clamp,              //!< `clamp(x, lo, hi)`
  Saturate,           //!< `saturate(x)`
  Fract,              //!< `fract(x)`
  Sqrt,               //!< `sqrt(x)`
  Length,             //!< `length(v)`
  Fwidth,             //!< `fwidth(x)` (fragment stage)
  Round,              //!< `round(x)`
  Select,             //!< `select(falseVal, trueVal, cond)`
  TextureSample,      //!< `textureSample(texture, sampler, coords)`
  TextureLoad,        //!< `textureLoad(texture, coords, level)`
  TextureDimensions,  //!< `textureDimensions(texture)`
};

/// Ostream output operator, e.g. `clamp`. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, BuiltinFn value);

/// What a name reference refers to; determines mutability and serialization.
enum class RefKind : uint8_t {
  Param,     //!< Function parameter (immutable).
  Let,       //!< Function-scope immutable binding.
  Var,       //!< Function-scope mutable variable (assignable).
  Constant,  //!< Module-scope constant.
  Resource,  //!< Module-scope resource binding.
};

/// Ostream output operator, e.g. `var`. @param os Output stream. @param value Value to output.
std::ostream& operator<<(std::ostream& os, RefKind value);

/**
 * A typed, immutable IR expression handle.
 *
 * Construct through the free factory functions (\ref LiteralF32, \ref Add, \ref Member,
 * \ref CallBuiltin, ...) or through the function builder's `ref`/`addLet`/`addVar`. Every handle
 * carries its result type; assignability is tracked so only `var`-rooted access chains can be
 * assignment targets.
 */
class IrExpr {
public:
  /// Expression node kind.
  enum class Kind : uint8_t {
    Literal,      //!< Scalar literal.
    Ref,          //!< Named reference (param/let/var/constant/resource).
    Unary,        //!< Unary minus or logical not.
    Binary,       //!< Binary operator.
    Member,       //!< Struct member access.
    Swizzle,      //!< Vector swizzle (.x, .xy, ...).
    Index,        //!< Array or vector indexing.
    Construct,    //!< Vector or matrix constructor.
    Convert,      //!< Scalar/vector element type conversion.
    CallBuiltin,  //!< Builtin function call.
    CallUser,     //!< User-defined function call.
  };

  /// Unary operators.
  enum class UnaryOp : uint8_t {
    Neg,  //!< Unary minus (numeric, non-u32).
    Not,  //!< Logical not (bool).
  };

  /// Result type of this expression.
  const IrType& type() const;

  /// Node kind.
  Kind kind() const;

  /// True if this expression is a valid assignment target: a `var` reference or a
  /// member/index/single-component-swizzle chain rooted at one.
  bool isMutableLvalue() const;

  /// One name reference found inside an expression tree (see \ref collectRefs).
  struct RefInfo {
    RefKind kind;   //!< What the name refers to.
    RcString name;  //!< Referenced name.
    IrType type;    //!< Type the reference was created with.
  };

  /**
   * Appends every name reference in this expression tree (depth-first, in operand order) to
   * \p out. Used by the function builder to re-verify that referenced names are still in scope
   * when an expression is recorded into a statement.
   *
   * @param out Destination list.
   */
  void collectRefs(std::vector<RefInfo>& out) const;

  /**
   * Appends every builtin function called anywhere in this expression tree to \p out. Used by
   * the function builder to enforce stage restrictions (fragment-only builtins).
   *
   * @param out Destination list.
   */
  void collectBuiltinCalls(std::vector<BuiltinFn>& out) const;

  /**
   * Appends the name of every user function called anywhere in this expression tree to \p out.
   * Used to propagate stage restrictions through user calls.
   *
   * @param out Destination list.
   */
  void collectUserCalls(std::vector<RcString>& out) const;

  /// Formats this expression as a deterministic prefix string, e.g. `add(ref(a), lit_f32(1))`.
  std::string toString() const;

  /**
   * Ostream output operator.
   *
   * @param os Output stream.
   * @param expr Expression to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const IrExpr& expr) {
    return os << expr.toString();
  }

#ifndef DOXYGEN_SHOULD_SKIP_THIS
  // Internal node storage; public for the factory/serialization implementation only.
  struct Node;
  explicit IrExpr(std::shared_ptr<const Node> node) : node_(std::move(node)) {}
  const Node& node() const { return *node_; }
#endif  // DOXYGEN_SHOULD_SKIP_THIS

private:
  std::shared_ptr<const Node> node_;
};

/// Internal storage for \ref IrExpr nodes; public for the factory, serialization, and emitter
/// implementations only.
struct IrExpr::Node {
  Kind kind = Kind::Literal;                                    //!< Node kind.
  IrType type = IrType::F32();                                  //!< Result type.
  bool mutableLvalue = false;                                   //!< Assignable access chain.
  std::variant<bool, int32_t, uint32_t, float> literal = 0.0f;  //!< Literal payload.
  RefKind refKind = RefKind::Let;                               //!< Ref payload.
  RcString name;                                                //!< Ref/member/callee name.
  UnaryOp unaryOp = UnaryOp::Neg;                               //!< Unary payload.
  BinaryOp binaryOp = BinaryOp::Add;                            //!< Binary payload.
  BuiltinFn builtin = BuiltinFn::Abs;                           //!< Builtin payload.
  std::string swizzle;                                          //!< Swizzle components.
  std::vector<IrExpr> children;                                 //!< Operands, in order.
};

/// `f32` literal. @param value Literal value.
IrExpr LiteralF32(float value);
/// `i32` literal. @param value Literal value.
IrExpr LiteralI32(int32_t value);
/// `u32` literal. @param value Literal value.
IrExpr LiteralU32(uint32_t value);
/// `bool` literal. @param value Literal value.
IrExpr LiteralBool(bool value);

/**
 * Named reference; normally produced by the function/module builders, which guarantee the name
 * exists with the given type.
 *
 * @param kind What the name refers to.
 * @param name Referenced name.
 * @param type Type of the referenced value.
 */
IrExpr MakeRef(RefKind kind, const RcString& name, const IrType& type);

/// `lhs + rhs` for matching numeric scalar/vector types.
/// @param lhs Left operand. @param rhs Right operand. @param label Diagnostic label.
ShaderResult<IrExpr> Add(const IrExpr& lhs, const IrExpr& rhs, const RcString& label = "add");
/// `lhs - rhs` for matching numeric scalar/vector types.
/// @param lhs Left operand. @param rhs Right operand. @param label Diagnostic label.
ShaderResult<IrExpr> Sub(const IrExpr& lhs, const IrExpr& rhs, const RcString& label = "sub");
/**
 * `lhs * rhs`: matching numeric scalar/vector types, vector*scalar, scalar*vector,
 * mat4x4f*vec4f, or mat4x4f*mat4x4f (required by the solid-fill vertex stage, which composes
 * the per-instance matrix with the MVP).
 *
 * @param lhs Left operand. @param rhs Right operand. @param label Diagnostic label.
 */
ShaderResult<IrExpr> Mul(const IrExpr& lhs, const IrExpr& rhs, const RcString& label = "mul");
/// `lhs / rhs` for matching numeric scalar/vector types.
/// @param lhs Left operand. @param rhs Right operand. @param label Diagnostic label.
ShaderResult<IrExpr> Div(const IrExpr& lhs, const IrExpr& rhs, const RcString& label = "div");

/// `lhs < rhs` for matching numeric scalar types; yields bool.
/// @param lhs Left operand. @param rhs Right operand. @param label Diagnostic label.
ShaderResult<IrExpr> Lt(const IrExpr& lhs, const IrExpr& rhs, const RcString& label = "lt");
/// `lhs <= rhs` for matching numeric scalar types; yields bool.
/// @param lhs Left operand. @param rhs Right operand. @param label Diagnostic label.
ShaderResult<IrExpr> Le(const IrExpr& lhs, const IrExpr& rhs, const RcString& label = "le");
/// `lhs > rhs` for matching numeric scalar types; yields bool.
/// @param lhs Left operand. @param rhs Right operand. @param label Diagnostic label.
ShaderResult<IrExpr> Gt(const IrExpr& lhs, const IrExpr& rhs, const RcString& label = "gt");
/// `lhs >= rhs` for matching numeric scalar types; yields bool.
/// @param lhs Left operand. @param rhs Right operand. @param label Diagnostic label.
ShaderResult<IrExpr> Ge(const IrExpr& lhs, const IrExpr& rhs, const RcString& label = "ge");
/// `lhs == rhs` for matching scalar types; yields bool.
/// @param lhs Left operand. @param rhs Right operand. @param label Diagnostic label.
ShaderResult<IrExpr> Eq(const IrExpr& lhs, const IrExpr& rhs, const RcString& label = "eq");
/// `lhs != rhs` for matching scalar types; yields bool.
/// @param lhs Left operand. @param rhs Right operand. @param label Diagnostic label.
ShaderResult<IrExpr> Ne(const IrExpr& lhs, const IrExpr& rhs, const RcString& label = "ne");

/// `lhs && rhs` for bools.
/// @param lhs Left operand. @param rhs Right operand. @param label Diagnostic label.
ShaderResult<IrExpr> And(const IrExpr& lhs, const IrExpr& rhs, const RcString& label = "and");
/// `lhs || rhs` for bools.
/// @param lhs Left operand. @param rhs Right operand. @param label Diagnostic label.
ShaderResult<IrExpr> Or(const IrExpr& lhs, const IrExpr& rhs, const RcString& label = "or");
/// `!operand` for bool.
/// @param operand Operand. @param label Diagnostic label.
ShaderResult<IrExpr> Not(const IrExpr& operand, const RcString& label = "not");
/// `-operand` for f32/i32 scalars and vectors (u32 has no unary minus).
/// @param operand Operand. @param label Diagnostic label.
ShaderResult<IrExpr> Neg(const IrExpr& operand, const RcString& label = "neg");

/// Struct member access `base.member`.
/// @param base Struct-typed expression. @param memberName Member to access.
/// @param label Diagnostic label.
ShaderResult<IrExpr> Member(const IrExpr& base, const RcString& memberName,
                            const RcString& label = "member");

/// Vector swizzle `base.xy` (components from xyzw, 1-4 of them, within the vector size).
/// @param base Vector-typed expression. @param components Swizzle string, e.g. `"xy"`.
/// @param label Diagnostic label.
ShaderResult<IrExpr> Swizzle(const IrExpr& base, std::string_view components,
                             const RcString& label = "swizzle");

/// Indexing `base[index]` for sized arrays, runtime arrays, and vectors, with an i32/u32 index.
/// @param base Indexable expression. @param index Index expression. @param label Diagnostic
/// label.
ShaderResult<IrExpr> Index(const IrExpr& base, const IrExpr& index,
                           const RcString& label = "index");

/**
 * Vector constructor: `vecN<T>(...)` from scalars and smaller vectors of element type T totaling
 * exactly N components, or a single scalar splat.
 *
 * @param target Vector type to construct.
 * @param args Constructor arguments.
 * @param label Diagnostic label.
 */
ShaderResult<IrExpr> ConstructVector(const IrType& target, std::vector<IrExpr> args,
                                     const RcString& label = "construct");

/**
 * Matrix constructor: `mat4x4f(c0, c1, c2, c3)` from four vec4f columns.
 *
 * @param columns The four column vectors.
 * @param label Diagnostic label.
 */
ShaderResult<IrExpr> ConstructMat4x4f(std::vector<IrExpr> columns,
                                      const RcString& label = "mat4x4f");

/**
 * Scalar or vector element-type conversion, e.g. `f32(x)`, `u32(x)`, `vec2i(vec2f)`. The target
 * must be a numeric scalar or vector; vector conversions require matching component counts, and
 * a numeric scalar argument may also convert to a vector by splat (e.g. `vec2i(0)`).
 *
 * @param target Conversion target type.
 * @param value Value to convert.
 * @param label Diagnostic label.
 */
ShaderResult<IrExpr> Convert(const IrType& target, const IrExpr& value,
                             const RcString& label = "convert");

/**
 * Builtin function call by enum.
 *
 * @param fn Builtin to call.
 * @param args Call arguments.
 * @param label Diagnostic label.
 */
ShaderResult<IrExpr> CallBuiltin(BuiltinFn fn, std::vector<IrExpr> args,
                                 const RcString& label = "call");

/**
 * Builtin function call by name; unknown names fail closed (only the solid-fill builtin subset
 * exists).
 *
 * @param name Builtin name, e.g. `"clamp"`.
 * @param args Call arguments.
 * @param label Diagnostic label.
 */
ShaderResult<IrExpr> CallBuiltinNamed(std::string_view name, std::vector<IrExpr> args,
                                      const RcString& label = "call");

/**
 * User-defined function call; normally produced by the function builder, which checks the
 * signature against the module's registered functions.
 *
 * @param name Callee name.
 * @param returnType Callee return type.
 * @param args Call arguments (already checked against the signature).
 */
IrExpr MakeCallUser(const RcString& name, const IrType& returnType, std::vector<IrExpr> args);

}  // namespace donner::gpu::shader
