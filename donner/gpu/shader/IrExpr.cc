#include "donner/gpu/shader/IrExpr.h"

#include <format>
#include <utility>
#include <variant>

namespace donner::gpu::shader {

namespace {

/// Builds a node-backed expression.
IrExpr MakeNode(IrExpr::Node&& node) {
  return IrExpr(std::make_shared<const IrExpr::Node>(std::move(node)));
}

/// Formats "type" for diagnostics.
std::string TypeName(const IrExpr& expr) {
  return expr.type().toString();
}

/// Shared checker for same-type numeric arithmetic.
ShaderResult<IrExpr> MakeArithmetic(BinaryOp op, const IrExpr& lhs, const IrExpr& rhs,
                                    const RcString& label) {
  if (!(lhs.type() == rhs.type()) || !lhs.type().isNumeric()) {
    return ShaderError{std::format("operands must be matching numeric scalar/vector types, got "
                                   "{} and {}",
                                   TypeName(lhs), TypeName(rhs)),
                       label};
  }

  IrExpr::Node node;
  node.kind = IrExpr::Kind::Binary;
  node.type = lhs.type();
  node.binaryOp = op;
  node.children = {lhs, rhs};
  return MakeNode(std::move(node));
}

/// Shared checker for ordered comparisons (numeric scalars) and equality (any scalars).
ShaderResult<IrExpr> MakeComparison(BinaryOp op, bool requireNumeric, const IrExpr& lhs,
                                    const IrExpr& rhs, const RcString& label) {
  const bool typeOk = requireNumeric ? lhs.type().isNumericScalar() : lhs.type().isScalar();
  if (!(lhs.type() == rhs.type()) || !typeOk) {
    return ShaderError{std::format("comparison operands must be matching {} scalar types, got "
                                   "{} and {}",
                                   requireNumeric ? "numeric" : "", TypeName(lhs), TypeName(rhs)),
                       label};
  }

  IrExpr::Node node;
  node.kind = IrExpr::Kind::Binary;
  node.type = IrType::Bool();
  node.binaryOp = op;
  node.children = {lhs, rhs};
  return MakeNode(std::move(node));
}

/// Shared checker for bool logical binary operators.
ShaderResult<IrExpr> MakeLogical(BinaryOp op, const IrExpr& lhs, const IrExpr& rhs,
                                 const RcString& label) {
  if (!(lhs.type() == IrType::Bool()) || !(rhs.type() == IrType::Bool())) {
    return ShaderError{
        std::format("logical operands must be bool, got {} and {}", TypeName(lhs), TypeName(rhs)),
        label};
  }

  IrExpr::Node node;
  node.kind = IrExpr::Kind::Binary;
  node.type = IrType::Bool();
  node.binaryOp = op;
  node.children = {lhs, rhs};
  return MakeNode(std::move(node));
}

/// Formats a literal payload deterministically.
std::string LiteralToString(const IrExpr::Node& node) {
  if (std::holds_alternative<bool>(node.literal)) {
    return std::get<bool>(node.literal) ? "lit_bool(true)" : "lit_bool(false)";
  }
  if (std::holds_alternative<int32_t>(node.literal)) {
    return std::format("lit_i32({})", std::get<int32_t>(node.literal));
  }
  if (std::holds_alternative<uint32_t>(node.literal)) {
    return std::format("lit_u32({})", std::get<uint32_t>(node.literal));
  }
  return std::format("lit_f32({})", std::get<float>(node.literal));
}

}  // namespace

std::ostream& operator<<(std::ostream& os, BinaryOp value) {
  switch (value) {
    case BinaryOp::Add: return os << "add";
    case BinaryOp::Sub: return os << "sub";
    case BinaryOp::Mul: return os << "mul";
    case BinaryOp::Div: return os << "div";
    case BinaryOp::Lt: return os << "lt";
    case BinaryOp::Le: return os << "le";
    case BinaryOp::Gt: return os << "gt";
    case BinaryOp::Ge: return os << "ge";
    case BinaryOp::Eq: return os << "eq";
    case BinaryOp::Ne: return os << "ne";
    case BinaryOp::And: return os << "and";
    case BinaryOp::Or: return os << "or";
  }
  return os << "unknown";
}

std::ostream& operator<<(std::ostream& os, BuiltinFn value) {
  switch (value) {
    case BuiltinFn::Abs: return os << "abs";
    case BuiltinFn::Min: return os << "min";
    case BuiltinFn::Max: return os << "max";
    case BuiltinFn::Clamp: return os << "clamp";
    case BuiltinFn::Saturate: return os << "saturate";
    case BuiltinFn::Fract: return os << "fract";
    case BuiltinFn::Sqrt: return os << "sqrt";
    case BuiltinFn::Length: return os << "length";
    case BuiltinFn::Fwidth: return os << "fwidth";
    case BuiltinFn::Round: return os << "round";
    case BuiltinFn::Select: return os << "select";
    case BuiltinFn::TextureSample: return os << "textureSample";
    case BuiltinFn::TextureLoad: return os << "textureLoad";
    case BuiltinFn::TextureDimensions: return os << "textureDimensions";
  }
  return os << "unknown";
}

std::ostream& operator<<(std::ostream& os, RefKind value) {
  switch (value) {
    case RefKind::Param: return os << "param";
    case RefKind::Let: return os << "let";
    case RefKind::Var: return os << "var";
    case RefKind::Constant: return os << "const";
    case RefKind::Resource: return os << "resource";
  }
  return os << "unknown";
}

const IrType& IrExpr::type() const {
  return node_->type;
}

IrExpr::Kind IrExpr::kind() const {
  return node_->kind;
}

bool IrExpr::isMutableLvalue() const {
  return node_->mutableLvalue;
}

void IrExpr::collectRefs(std::vector<RefInfo>& out) const {
  if (node_->kind == Kind::Ref) {
    out.push_back(RefInfo{node_->refKind, node_->name, node_->type});
  }
  for (const IrExpr& child : node_->children) {
    child.collectRefs(out);
  }
}

void IrExpr::collectBuiltinCalls(std::vector<BuiltinFn>& out) const {
  if (node_->kind == Kind::CallBuiltin) {
    out.push_back(node_->builtin);
  }
  for (const IrExpr& child : node_->children) {
    child.collectBuiltinCalls(out);
  }
}

void IrExpr::collectUserCalls(std::vector<RcString>& out) const {
  if (node_->kind == Kind::CallUser) {
    out.push_back(node_->name);
  }
  for (const IrExpr& child : node_->children) {
    child.collectUserCalls(out);
  }
}

std::string IrExpr::toString() const {
  const Node& node = *node_;
  switch (node.kind) {
    case Kind::Literal: return LiteralToString(node);
    case Kind::Ref: return std::format("ref({})", node.name.str());
    case Kind::Unary:
      return std::format("{}({})", node.unaryOp == UnaryOp::Neg ? "neg" : "not",
                         node.children[0].toString());
    case Kind::Binary: {
      std::string result;
      switch (node.binaryOp) {
        case BinaryOp::Add: result = "add"; break;
        case BinaryOp::Sub: result = "sub"; break;
        case BinaryOp::Mul: result = "mul"; break;
        case BinaryOp::Div: result = "div"; break;
        case BinaryOp::Lt: result = "lt"; break;
        case BinaryOp::Le: result = "le"; break;
        case BinaryOp::Gt: result = "gt"; break;
        case BinaryOp::Ge: result = "ge"; break;
        case BinaryOp::Eq: result = "eq"; break;
        case BinaryOp::Ne: result = "ne"; break;
        case BinaryOp::And: result = "and"; break;
        case BinaryOp::Or: result = "or"; break;
      }
      return std::format("{}({}, {})", result, node.children[0].toString(),
                         node.children[1].toString());
    }
    case Kind::Member:
      return std::format("member({}, {})", node.children[0].toString(), node.name.str());
    case Kind::Swizzle:
      return std::format("swizzle({}, {})", node.children[0].toString(), node.swizzle);
    case Kind::Index:
      return std::format("index({}, {})", node.children[0].toString(), node.children[1].toString());
    case Kind::Construct:
    case Kind::CallUser:
    case Kind::CallBuiltin:
    case Kind::Convert: {
      std::string head;
      if (node.kind == Kind::Construct) {
        head = std::format("construct_{}", node.type.toString());
      } else if (node.kind == Kind::Convert) {
        head = std::format("convert_{}", node.type.toString());
      } else if (node.kind == Kind::CallUser) {
        head = std::format("call({}", node.name.str());
      } else {
        head = "builtin_";
        switch (node.builtin) {
          case BuiltinFn::Abs: head += "abs"; break;
          case BuiltinFn::Min: head += "min"; break;
          case BuiltinFn::Max: head += "max"; break;
          case BuiltinFn::Clamp: head += "clamp"; break;
          case BuiltinFn::Saturate: head += "saturate"; break;
          case BuiltinFn::Fract: head += "fract"; break;
          case BuiltinFn::Sqrt: head += "sqrt"; break;
          case BuiltinFn::Length: head += "length"; break;
          case BuiltinFn::Fwidth: head += "fwidth"; break;
          case BuiltinFn::Round: head += "round"; break;
          case BuiltinFn::Select: head += "select"; break;
          case BuiltinFn::TextureSample: head += "textureSample"; break;
          case BuiltinFn::TextureLoad: head += "textureLoad"; break;
          case BuiltinFn::TextureDimensions: head += "textureDimensions"; break;
        }
      }

      std::string result = node.kind == Kind::CallUser ? head : head + "(";
      for (size_t i = 0; i < node.children.size(); ++i) {
        if (i > 0 || node.kind == Kind::CallUser) {
          result += ", ";
        }
        result += node.children[i].toString();
      }
      result += ")";
      return result;
    }
  }
  return "unknown";
}

IrExpr LiteralF32(float value) {
  IrExpr::Node node;
  node.kind = IrExpr::Kind::Literal;
  node.type = IrType::F32();
  node.literal = value;
  return MakeNode(std::move(node));
}

IrExpr LiteralI32(int32_t value) {
  IrExpr::Node node;
  node.kind = IrExpr::Kind::Literal;
  node.type = IrType::I32();
  node.literal = value;
  return MakeNode(std::move(node));
}

IrExpr LiteralU32(uint32_t value) {
  IrExpr::Node node;
  node.kind = IrExpr::Kind::Literal;
  node.type = IrType::U32();
  node.literal = value;
  return MakeNode(std::move(node));
}

IrExpr LiteralBool(bool value) {
  IrExpr::Node node;
  node.kind = IrExpr::Kind::Literal;
  node.type = IrType::Bool();
  node.literal = value;
  return MakeNode(std::move(node));
}

IrExpr MakeRef(RefKind kind, const RcString& name, const IrType& type) {
  IrExpr::Node node;
  node.kind = IrExpr::Kind::Ref;
  node.type = type;
  node.refKind = kind;
  node.name = name;
  node.mutableLvalue = kind == RefKind::Var;
  return MakeNode(std::move(node));
}

ShaderResult<IrExpr> Add(const IrExpr& lhs, const IrExpr& rhs, const RcString& label) {
  return MakeArithmetic(BinaryOp::Add, lhs, rhs, label);
}
ShaderResult<IrExpr> Sub(const IrExpr& lhs, const IrExpr& rhs, const RcString& label) {
  return MakeArithmetic(BinaryOp::Sub, lhs, rhs, label);
}

ShaderResult<IrExpr> Mul(const IrExpr& lhs, const IrExpr& rhs, const RcString& label) {
  const IrType& lt = lhs.type();
  const IrType& rt = rhs.type();

  // mat4x4f * vec4f and mat4x4f * mat4x4f (the vertex stage composes matrices).
  if (lt.kind() == IrType::Kind::Matrix4x4f) {
    if (rt == IrType::Vec4f() || rt.kind() == IrType::Kind::Matrix4x4f) {
      IrExpr::Node node;
      node.kind = IrExpr::Kind::Binary;
      node.type = rt;
      node.binaryOp = BinaryOp::Mul;
      node.children = {lhs, rhs};
      return MakeNode(std::move(node));
    }
    return ShaderError{
        std::format("mat4x4<f32> can multiply vec4<f32> or mat4x4<f32>, got {}", rt.toString()),
        label};
  }

  // vector * scalar and scalar * vector with matching element type.
  const bool vectorScalar =
      lt.isNumericVector() && rt.isNumericScalar() && lt.scalarKind() == rt.scalarKind();
  const bool scalarVector =
      lt.isNumericScalar() && rt.isNumericVector() && lt.scalarKind() == rt.scalarKind();
  if (vectorScalar || scalarVector) {
    IrExpr::Node node;
    node.kind = IrExpr::Kind::Binary;
    node.type = vectorScalar ? lt : rt;
    node.binaryOp = BinaryOp::Mul;
    node.children = {lhs, rhs};
    return MakeNode(std::move(node));
  }

  return MakeArithmetic(BinaryOp::Mul, lhs, rhs, label);
}

ShaderResult<IrExpr> Div(const IrExpr& lhs, const IrExpr& rhs, const RcString& label) {
  // vector / broadcast-scalar and broadcast-scalar / vector with matching element type; the
  // solid-fill fragment stage needs the latter for `1.0 / fwidth(sample_pos)`.
  const IrType& lt = lhs.type();
  const IrType& rt = rhs.type();
  const bool vectorScalar =
      lt.isNumericVector() && rt.isNumericScalar() && lt.scalarKind() == rt.scalarKind();
  const bool scalarVector =
      lt.isNumericScalar() && rt.isNumericVector() && lt.scalarKind() == rt.scalarKind();
  if (vectorScalar || scalarVector) {
    IrExpr::Node node;
    node.kind = IrExpr::Kind::Binary;
    node.type = vectorScalar ? lt : rt;
    node.binaryOp = BinaryOp::Div;
    node.children = {lhs, rhs};
    return MakeNode(std::move(node));
  }

  return MakeArithmetic(BinaryOp::Div, lhs, rhs, label);
}

ShaderResult<IrExpr> Lt(const IrExpr& lhs, const IrExpr& rhs, const RcString& label) {
  return MakeComparison(BinaryOp::Lt, /*requireNumeric=*/true, lhs, rhs, label);
}
ShaderResult<IrExpr> Le(const IrExpr& lhs, const IrExpr& rhs, const RcString& label) {
  return MakeComparison(BinaryOp::Le, /*requireNumeric=*/true, lhs, rhs, label);
}
ShaderResult<IrExpr> Gt(const IrExpr& lhs, const IrExpr& rhs, const RcString& label) {
  return MakeComparison(BinaryOp::Gt, /*requireNumeric=*/true, lhs, rhs, label);
}
ShaderResult<IrExpr> Ge(const IrExpr& lhs, const IrExpr& rhs, const RcString& label) {
  return MakeComparison(BinaryOp::Ge, /*requireNumeric=*/true, lhs, rhs, label);
}
ShaderResult<IrExpr> Eq(const IrExpr& lhs, const IrExpr& rhs, const RcString& label) {
  return MakeComparison(BinaryOp::Eq, /*requireNumeric=*/false, lhs, rhs, label);
}
ShaderResult<IrExpr> Ne(const IrExpr& lhs, const IrExpr& rhs, const RcString& label) {
  return MakeComparison(BinaryOp::Ne, /*requireNumeric=*/false, lhs, rhs, label);
}

ShaderResult<IrExpr> And(const IrExpr& lhs, const IrExpr& rhs, const RcString& label) {
  return MakeLogical(BinaryOp::And, lhs, rhs, label);
}
ShaderResult<IrExpr> Or(const IrExpr& lhs, const IrExpr& rhs, const RcString& label) {
  return MakeLogical(BinaryOp::Or, lhs, rhs, label);
}

ShaderResult<IrExpr> Not(const IrExpr& operand, const RcString& label) {
  if (!(operand.type() == IrType::Bool())) {
    return ShaderError{std::format("logical not requires bool, got {}", TypeName(operand)), label};
  }
  IrExpr::Node node;
  node.kind = IrExpr::Kind::Unary;
  node.type = IrType::Bool();
  node.unaryOp = IrExpr::UnaryOp::Not;
  node.children = {operand};
  return MakeNode(std::move(node));
}

ShaderResult<IrExpr> Neg(const IrExpr& operand, const RcString& label) {
  if (!operand.type().isNumeric() || operand.type().scalarKind() == ScalarKind::U32) {
    return ShaderError{
        std::format("unary minus requires f32/i32 scalar or vector, got {}", TypeName(operand)),
        label};
  }
  IrExpr::Node node;
  node.kind = IrExpr::Kind::Unary;
  node.type = operand.type();
  node.unaryOp = IrExpr::UnaryOp::Neg;
  node.children = {operand};
  return MakeNode(std::move(node));
}

ShaderResult<IrExpr> Member(const IrExpr& base, const RcString& memberName, const RcString& label) {
  if (base.type().kind() != IrType::Kind::Struct) {
    return ShaderError{std::format("member access requires a struct, got {}", TypeName(base)),
                       label};
  }
  for (const IrType::Member& member : base.type().structMembers()) {
    if (member.name == memberName) {
      IrExpr::Node node;
      node.kind = IrExpr::Kind::Member;
      node.type = member.type;
      node.name = memberName;
      node.mutableLvalue = base.isMutableLvalue();
      node.children = {base};
      return MakeNode(std::move(node));
    }
  }
  return ShaderError{std::format("struct {} has no member named {}", base.type().structName().str(),
                                 memberName.str()),
                     label};
}

ShaderResult<IrExpr> Swizzle(const IrExpr& base, std::string_view components,
                             const RcString& label) {
  if (!base.type().isVector()) {
    return ShaderError{std::format("swizzle requires a vector, got {}", TypeName(base)), label};
  }
  if (components.empty() || components.size() > 4) {
    return ShaderError{std::format("swizzle must have 1-4 components, got \"{}\"", components),
                       label};
  }
  for (const char component : components) {
    const uint32_t componentIndex = component == 'x'   ? 0
                                    : component == 'y' ? 1
                                    : component == 'z' ? 2
                                    : component == 'w' ? 3
                                                       : 4;
    if (componentIndex >= base.type().vectorSize()) {
      return ShaderError{
          std::format("swizzle component '{}' is out of range for {}", component, TypeName(base)),
          label};
    }
  }

  const ScalarKind element = base.type().scalarKind();
  IrExpr::Node node;
  node.kind = IrExpr::Kind::Swizzle;
  node.type = components.size() == 1 ? IrType::Scalar(element)
                                     : (components.size() == 2   ? IrType::Vec2(element)
                                        : components.size() == 3 ? IrType::Vec3(element)
                                                                 : IrType::Vec4(element));
  node.swizzle = std::string(components);
  // Only a single-component swizzle of a mutable chain is assignable.
  node.mutableLvalue = base.isMutableLvalue() && components.size() == 1;
  node.children = {base};
  return MakeNode(std::move(node));
}

ShaderResult<IrExpr> Index(const IrExpr& base, const IrExpr& index, const RcString& label) {
  if (!(index.type() == IrType::I32()) && !(index.type() == IrType::U32())) {
    return ShaderError{std::format("index must be i32 or u32, got {}", TypeName(index)), label};
  }

  const IrType& baseType = base.type();
  if (baseType.kind() == IrType::Kind::SizedArray ||
      baseType.kind() == IrType::Kind::RuntimeArray) {
    IrExpr::Node node;
    node.kind = IrExpr::Kind::Index;
    node.type = baseType.elementType();
    node.mutableLvalue = base.isMutableLvalue();
    node.children = {base, index};
    return MakeNode(std::move(node));
  }
  if (baseType.isVector()) {
    IrExpr::Node node;
    node.kind = IrExpr::Kind::Index;
    node.type = IrType::Scalar(baseType.scalarKind());
    node.mutableLvalue = base.isMutableLvalue();
    node.children = {base, index};
    return MakeNode(std::move(node));
  }
  return ShaderError{
      std::format("type {} is not indexable (arrays and vectors are)", TypeName(base)), label};
}

ShaderResult<IrExpr> ConstructVector(const IrType& target, std::vector<IrExpr> args,
                                     const RcString& label) {
  if (!target.isVector()) {
    return ShaderError{std::format("constructor target {} is not a vector type", target.toString()),
                       label};
  }
  if (args.empty()) {
    return ShaderError{"vector constructor requires at least one argument", label};
  }

  const ScalarKind element = target.scalarKind();

  // Single scalar splat.
  const bool isSplat = args.size() == 1 && args[0].type().isScalar();
  uint32_t componentCount = 0;
  for (const IrExpr& arg : args) {
    const IrType& argType = arg.type();
    if (argType.isScalar() && argType.scalarKind() == element) {
      componentCount += 1;
    } else if (argType.isVector() && argType.scalarKind() == element) {
      componentCount += argType.vectorSize();
    } else if (!isSplat) {
      return ShaderError{std::format("constructor argument type {} does not match target {}",
                                     argType.toString(), target.toString()),
                         label};
    }
  }
  if (isSplat) {
    if (!(args[0].type().isScalar() && args[0].type().scalarKind() == element)) {
      return ShaderError{std::format("splat argument type {} does not match target {}",
                                     args[0].type().toString(), target.toString()),
                         label};
    }
  } else if (componentCount != target.vectorSize()) {
    return ShaderError{std::format("constructor for {} needs {} components, got {}",
                                   target.toString(), target.vectorSize(), componentCount),
                       label};
  }

  IrExpr::Node node;
  node.kind = IrExpr::Kind::Construct;
  node.type = target;
  node.children = std::move(args);
  return MakeNode(std::move(node));
}

ShaderResult<IrExpr> ConstructMat4x4f(std::vector<IrExpr> columns, const RcString& label) {
  if (columns.size() != 4) {
    return ShaderError{
        std::format("mat4x4<f32> constructor needs 4 columns, got {}", columns.size()), label};
  }
  for (const IrExpr& column : columns) {
    if (!(column.type() == IrType::Vec4f())) {
      return ShaderError{
          std::format("mat4x4<f32> columns must be vec4<f32>, got {}", column.type().toString()),
          label};
    }
  }

  IrExpr::Node node;
  node.kind = IrExpr::Kind::Construct;
  node.type = IrType::Mat4x4f();
  node.children = std::move(columns);
  return MakeNode(std::move(node));
}

ShaderResult<IrExpr> Convert(const IrType& target, const IrExpr& value, const RcString& label) {
  const bool targetNumeric = target.isNumericScalar() || target.isNumericVector();
  if (!targetNumeric) {
    return ShaderError{
        std::format("conversion target {} must be a numeric scalar or vector", target.toString()),
        label};
  }

  const IrType& sourceType = value.type();
  const bool scalarToScalar = target.isNumericScalar() && sourceType.isNumericScalar();
  const bool vectorToVector = target.isNumericVector() && sourceType.isNumericVector() &&
                              target.vectorSize() == sourceType.vectorSize();
  const bool scalarSplat = target.isNumericVector() && sourceType.isNumericScalar();
  if (!scalarToScalar && !vectorToVector && !scalarSplat) {
    return ShaderError{
        std::format("cannot convert {} to {}", sourceType.toString(), target.toString()), label};
  }

  IrExpr::Node node;
  node.kind = IrExpr::Kind::Convert;
  node.type = target;
  node.children = {value};
  return MakeNode(std::move(node));
}

namespace {

/// Type-checks builtin arguments and computes the result type.
ShaderResult<IrType> CheckBuiltin(BuiltinFn fn, std::span<const IrExpr> args,
                                  const RcString& label) {
  const auto argCountError = [&](size_t expected) {
    return ShaderError{std::format("builtin expects {} arguments, got {}", expected, args.size()),
                       label};
  };

  switch (fn) {
    case BuiltinFn::Abs:
      if (args.size() != 1) return argCountError(1);
      if (!args[0].type().isNumeric()) {
        return ShaderError{std::format("abs requires a numeric type, got {}", TypeName(args[0])),
                           label};
      }
      return args[0].type();

    case BuiltinFn::Min:
    case BuiltinFn::Max:
      if (args.size() != 2) return argCountError(2);
      if (!(args[0].type() == args[1].type()) || !args[0].type().isNumeric()) {
        return ShaderError{std::format("min/max require matching numeric types, got {} and {}",
                                       TypeName(args[0]), TypeName(args[1])),
                           label};
      }
      return args[0].type();

    case BuiltinFn::Clamp:
      if (args.size() != 3) return argCountError(3);
      if (!(args[0].type() == args[1].type()) || !(args[0].type() == args[2].type()) ||
          !args[0].type().isNumeric()) {
        return ShaderError{
            std::format("clamp requires three matching numeric operands, got {}, {}, {}",
                        TypeName(args[0]), TypeName(args[1]), TypeName(args[2])),
            label};
      }
      return args[0].type();

    case BuiltinFn::Saturate:
    case BuiltinFn::Fract:
    case BuiltinFn::Sqrt:
    case BuiltinFn::Fwidth:
    case BuiltinFn::Round:
      if (args.size() != 1) return argCountError(1);
      if (!args[0].type().isFloatScalarOrVector()) {
        return ShaderError{
            std::format("builtin requires f32 scalar or vector, got {}", TypeName(args[0])), label};
      }
      return args[0].type();

    case BuiltinFn::Length:
      if (args.size() != 1) return argCountError(1);
      if (!args[0].type().isVector() || args[0].type().scalarKind() != ScalarKind::F32) {
        return ShaderError{std::format("length requires a float vector, got {}", TypeName(args[0])),
                           label};
      }
      return IrType::F32();

    case BuiltinFn::Select:
      if (args.size() != 3) return argCountError(3);
      if (!(args[0].type() == args[1].type())) {
        return ShaderError{std::format("select branches must have matching types, got {} and {}",
                                       TypeName(args[0]), TypeName(args[1])),
                           label};
      }
      if (!(args[2].type() == IrType::Bool())) {
        return ShaderError{std::format("select condition must be bool, got {}", TypeName(args[2])),
                           label};
      }
      return args[0].type();

    case BuiltinFn::TextureSample:
      if (args.size() != 3) return argCountError(3);
      if (args[0].type().kind() != IrType::Kind::Texture2dF32 ||
          args[1].type().kind() != IrType::Kind::Sampler || !(args[2].type() == IrType::Vec2f())) {
        return ShaderError{
            std::format("textureSample requires (texture_2d<f32>, sampler, vec2<f32>), got "
                        "({}, {}, {})",
                        TypeName(args[0]), TypeName(args[1]), TypeName(args[2])),
            label};
      }
      return IrType::Vec4f();

    case BuiltinFn::TextureLoad:
      if (args.size() != 3) return argCountError(3);
      if (args[0].type().kind() != IrType::Kind::Texture2dF32 ||
          !(args[1].type() == IrType::Vec2i()) ||
          (!(args[2].type() == IrType::I32()) && !(args[2].type() == IrType::U32()))) {
        return ShaderError{
            std::format("textureLoad requires (texture_2d<f32>, vec2<i32>, i32|u32), got "
                        "({}, {}, {})",
                        TypeName(args[0]), TypeName(args[1]), TypeName(args[2])),
            label};
      }
      return IrType::Vec4f();

    case BuiltinFn::TextureDimensions:
      if (args.size() != 1) return argCountError(1);
      if (args[0].type().kind() != IrType::Kind::Texture2dF32) {
        return ShaderError{
            std::format("textureDimensions requires a texture, got {}", TypeName(args[0])), label};
      }
      return IrType::Vec2u();
  }

  return ShaderError{"unknown builtin", label};
}

}  // namespace

ShaderResult<IrExpr> CallBuiltin(BuiltinFn fn, std::vector<IrExpr> args, const RcString& label) {
  ShaderResult<IrType> resultType = CheckBuiltin(fn, args, label);
  if (resultType.hasError()) {
    return std::move(resultType).error();
  }

  IrExpr::Node node;
  node.kind = IrExpr::Kind::CallBuiltin;
  node.type = std::move(resultType).result();
  node.builtin = fn;
  node.children = std::move(args);
  return MakeNode(std::move(node));
}

ShaderResult<IrExpr> CallBuiltinNamed(std::string_view name, std::vector<IrExpr> args,
                                      const RcString& label) {
  static constexpr std::pair<std::string_view, BuiltinFn> kBuiltins[] = {
      {"abs", BuiltinFn::Abs},
      {"min", BuiltinFn::Min},
      {"max", BuiltinFn::Max},
      {"clamp", BuiltinFn::Clamp},
      {"saturate", BuiltinFn::Saturate},
      {"fract", BuiltinFn::Fract},
      {"sqrt", BuiltinFn::Sqrt},
      {"length", BuiltinFn::Length},
      {"fwidth", BuiltinFn::Fwidth},
      {"round", BuiltinFn::Round},
      {"select", BuiltinFn::Select},
      {"textureSample", BuiltinFn::TextureSample},
      {"textureLoad", BuiltinFn::TextureLoad},
      {"textureDimensions", BuiltinFn::TextureDimensions},
  };
  for (const auto& [builtinName, fn] : kBuiltins) {
    if (builtinName == name) {
      return CallBuiltin(fn, std::move(args), label);
    }
  }
  return ShaderError{std::format("unknown builtin function \"{}\"", name), label};
}

IrExpr MakeCallUser(const RcString& name, const IrType& returnType, std::vector<IrExpr> args) {
  IrExpr::Node node;
  node.kind = IrExpr::Kind::CallUser;
  node.type = returnType;
  node.name = name;
  node.children = std::move(args);
  return MakeNode(std::move(node));
}

}  // namespace donner::gpu::shader
