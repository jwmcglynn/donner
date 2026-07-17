#include "donner/gpu/shader/WgslEmitter.h"

#include <cmath>
#include <format>
#include <string_view>
#include <variant>
#include <vector>

#include "donner/gpu/shader/IrLayout.h"

namespace donner::gpu::shader {

namespace {

/// WGSL reserved and predeclared words that IR identifiers must not collide with. Small,
/// deliberately conservative subset covering keywords, types, address spaces, and the builtin
/// functions this IR can call.
constexpr std::string_view kReservedWords[] = {
    "alias",
    "array",
    "bitcast",
    "bool",
    "break",
    "case",
    "const",
    "continue",
    "continuing",
    "default",
    "discard",
    "else",
    "enable",
    "f16",
    "f32",
    "false",
    "fn",
    "for",
    "fragment",
    "i32",
    "if",
    "let",
    "loop",
    "mat2x2",
    "mat3x3",
    "mat4x4",
    "override",
    "ptr",
    "read",
    "read_write",
    "ref",
    "requires",
    "return",
    "sampler",
    "storage",
    "struct",
    "switch",
    "texture_2d",
    "true",
    "u32",
    "uniform",
    "var",
    "vec2",
    "vec3",
    "vec4",
    "vertex",
    "while",
    "write",
    "abs",
    "clamp",
    "fract",
    "fwidth",
    "length",
    "max",
    "min",
    "round",
    "saturate",
    "select",
    "sqrt",
    "textureDimensions",
    "textureLoad",
    "textureSample",
    // WGSL keyword / reserved-word table entries beyond the subset above.
    "atomic",
    "attribute",
    "binding_array",
    "compute",
    "const_assert",
    "diagnostic",
    "do",
    "enum",
    "function",
    "handle",
    "private",
    "typedef",
    "using",
    "workgroup",
};

/// Checks an identifier for WGSL lexical validity and reserved-word collisions; fails closed.
/// Lexical enforcement lives in the emitters (not the target-neutral module builder): each
/// target language owns its own identifier rules, and the emitter is the last point where
/// invalid source could otherwise escape.
ShaderStatus CheckIdentifier(const RcString& name, std::string_view context) {
  if (name.empty()) {
    return ShaderError{std::format("{}: identifier is empty", context), "wgsl"};
  }

  const std::string_view text(name);
  const auto isIdentifierStart = [](char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
  };
  const auto isIdentifierChar = [&](char ch) {
    return isIdentifierStart(ch) || (ch >= '0' && ch <= '9');
  };
  bool lexicallyValid = isIdentifierStart(text[0]);
  for (size_t i = 1; lexicallyValid && i < text.size(); ++i) {
    lexicallyValid = isIdentifierChar(text[i]);
  }
  // WGSL reserves identifiers beginning with a double underscore.
  if (!lexicallyValid || text.starts_with("__")) {
    return ShaderError{
        std::format("{}: \"{}\" is not a valid identifier (WGSL identifiers match "
                    "[A-Za-z_][A-Za-z0-9_]* and may not start with a double underscore)",
                    context, name.str()),
        "wgsl"};
  }

  for (const std::string_view reserved : kReservedWords) {
    if (std::string_view(name) == reserved) {
      return ShaderError{std::format("{}: identifier \"{}\" collides with a WGSL reserved word",
                                     context, name.str()),
                         "wgsl"};
    }
  }
  return OkShaderStatus();
}

/// WGSL spelling of a type, e.g. `vec2<f32>` or `array<Band>`.
std::string TypeToWgsl(const IrType& type) {
  switch (type.kind()) {
    case IrType::Kind::Scalar:
      switch (type.scalarKind()) {
        case ScalarKind::Bool: return "bool";
        case ScalarKind::I32: return "i32";
        case ScalarKind::U32: return "u32";
        case ScalarKind::F32: return "f32";
      }
      return "f32";
    case IrType::Kind::Vector: {
      std::string element;
      switch (type.scalarKind()) {
        case ScalarKind::Bool: element = "bool"; break;
        case ScalarKind::I32: element = "i32"; break;
        case ScalarKind::U32: element = "u32"; break;
        case ScalarKind::F32: element = "f32"; break;
      }
      return std::format("vec{}<{}>", type.vectorSize(), element);
    }
    case IrType::Kind::Matrix4x4f: return "mat4x4<f32>";
    case IrType::Kind::SizedArray:
      return std::format("array<{}, {}>", TypeToWgsl(type.elementType()), type.arrayCount());
    case IrType::Kind::RuntimeArray:
      return std::format("array<{}>", TypeToWgsl(type.elementType()));
    case IrType::Kind::Struct: return type.structName().str();
    case IrType::Kind::Texture2dF32: return "texture_2d<f32>";
    case IrType::Kind::Sampler: return "sampler";
  }
  return "f32";
}

/// Emitter state: output text plus the first latched error.
class Emitter {
public:
  explicit Emitter(const IrModule& module) : module_(module) {}

  /// Runs emission. @return The WGSL text or the first error.
  ShaderResult<std::string> emit();

private:
  void latch(ShaderError&& error) {
    if (!error_) {
      error_ = std::move(error);
    }
  }
  void check(ShaderStatus&& status) {
    if (status.hasError()) {
      latch(std::move(status).error());
    }
  }

  void line(std::string text) {
    for (int i = 0; i < indent_; ++i) {
      out_ += "  ";
    }
    out_ += text;
    out_ += '\n';
  }
  void blank() { out_ += '\n'; }

  std::string literalToWgsl(const IrExpr::Node& node);
  std::string exprToWgsl(const IrExpr& expr);
  void emitStatement(const IrStmt& statement, const IrFunction& function);
  void emitBlock(const IrBlock& block, const IrFunction& function);

  void collectStructs(const IrType& type, std::vector<IrType>& out);
  void validateBindingLayout(const IrType& type, AddressSpace addressSpace);
  void emitStructDeclarations();
  void checkUniformArrays(const IrType& type);
  void emitConstants();
  void emitBindings();
  void emitFunction(const IrFunction& function);

  /// Generated output-struct name for an entry point.
  static std::string OutputStructName(const IrFunction& function) {
    return std::format("{}_Output", std::string_view(function.name));
  }

  const IrModule& module_;
  std::string out_;
  int indent_ = 0;
  std::vector<RcString> userStructNames_;
  std::optional<ShaderError> error_;
};

std::string Emitter::literalToWgsl(const IrExpr::Node& node) {
  if (std::holds_alternative<bool>(node.literal)) {
    return std::get<bool>(node.literal) ? "true" : "false";
  }
  if (std::holds_alternative<int32_t>(node.literal)) {
    return std::format("{}i", std::get<int32_t>(node.literal));
  }
  if (std::holds_alternative<uint32_t>(node.literal)) {
    return std::format("{}u", std::get<uint32_t>(node.literal));
  }

  const float value = std::get<float>(node.literal);
  if (!std::isfinite(value)) {
    latch(ShaderError{"non-finite float literals cannot be emitted as WGSL", "wgsl"});
    return "0f";
  }
  // Shortest round-trip formatting (same style as the IR serializer) with an explicit `f`
  // suffix so the literal is f32 regardless of context.
  return std::format("{}f", value);
}

std::string Emitter::exprToWgsl(const IrExpr& expr) {
  const IrExpr::Node& node = expr.node();
  switch (node.kind) {
    case IrExpr::Kind::Literal: return literalToWgsl(node);
    case IrExpr::Kind::Ref: return node.name.str();
    case IrExpr::Kind::Unary:
      return std::format("({}{})", node.unaryOp == IrExpr::UnaryOp::Neg ? "-" : "!",
                         exprToWgsl(node.children[0]));
    case IrExpr::Kind::Binary: {
      std::string_view op;
      switch (node.binaryOp) {
        case BinaryOp::Add: op = "+"; break;
        case BinaryOp::Sub: op = "-"; break;
        case BinaryOp::Mul: op = "*"; break;
        case BinaryOp::Div: op = "/"; break;
        case BinaryOp::Lt: op = "<"; break;
        case BinaryOp::Le: op = "<="; break;
        case BinaryOp::Gt: op = ">"; break;
        case BinaryOp::Ge: op = ">="; break;
        case BinaryOp::Eq: op = "=="; break;
        case BinaryOp::Ne: op = "!="; break;
        case BinaryOp::And: op = "&&"; break;
        case BinaryOp::Or: op = "||"; break;
      }
      return std::format("({} {} {})", exprToWgsl(node.children[0]), op,
                         exprToWgsl(node.children[1]));
    }
    case IrExpr::Kind::Member:
      return std::format("{}.{}", exprToWgsl(node.children[0]), node.name.str());
    case IrExpr::Kind::Swizzle:
      return std::format("{}.{}", exprToWgsl(node.children[0]), node.swizzle);
    case IrExpr::Kind::Index:
      return std::format("{}[{}]", exprToWgsl(node.children[0]), exprToWgsl(node.children[1]));
    case IrExpr::Kind::Construct:
    case IrExpr::Kind::Convert: {
      std::string result = TypeToWgsl(node.type);
      result += "(";
      for (size_t i = 0; i < node.children.size(); ++i) {
        if (i > 0) {
          result += ", ";
        }
        result += exprToWgsl(node.children[i]);
      }
      result += ")";
      return result;
    }
    case IrExpr::Kind::CallBuiltin: {
      std::string name;
      switch (node.builtin) {
        case BuiltinFn::Abs: name = "abs"; break;
        case BuiltinFn::Min: name = "min"; break;
        case BuiltinFn::Max: name = "max"; break;
        case BuiltinFn::Clamp: name = "clamp"; break;
        case BuiltinFn::Saturate: name = "saturate"; break;
        case BuiltinFn::Fract: name = "fract"; break;
        case BuiltinFn::Sqrt: name = "sqrt"; break;
        case BuiltinFn::Length: name = "length"; break;
        case BuiltinFn::Fwidth: name = "fwidth"; break;
        case BuiltinFn::Round: name = "round"; break;
        case BuiltinFn::Select: name = "select"; break;
        case BuiltinFn::TextureSample: name = "textureSample"; break;
        case BuiltinFn::TextureLoad: name = "textureLoad"; break;
        case BuiltinFn::TextureDimensions: name = "textureDimensions"; break;
      }
      std::string result = name + "(";
      for (size_t i = 0; i < node.children.size(); ++i) {
        if (i > 0) {
          result += ", ";
        }
        result += exprToWgsl(node.children[i]);
      }
      result += ")";
      return result;
    }
    case IrExpr::Kind::CallUser: {
      std::string result = node.name.str() + "(";
      for (size_t i = 0; i < node.children.size(); ++i) {
        if (i > 0) {
          result += ", ";
        }
        result += exprToWgsl(node.children[i]);
      }
      result += ")";
      return result;
    }
  }
  return "0f";
}

void Emitter::emitStatement(const IrStmt& statement, const IrFunction& function) {
  const IrStmt::Data& data = statement.data();
  switch (statement.kind()) {
    case IrStmt::Kind::Let:
      check(CheckIdentifier(data.name, "let"));
      line(std::format("let {} = {};", data.name.str(), exprToWgsl(data.exprs[0])));
      return;
    case IrStmt::Kind::Var:
      check(CheckIdentifier(data.name, "var"));
      if (!data.exprs.empty()) {
        line(std::format("var {}: {} = {};", data.name.str(), TypeToWgsl(*data.declaredType),
                         exprToWgsl(data.exprs[0])));
      } else {
        line(std::format("var {}: {};", data.name.str(), TypeToWgsl(*data.declaredType)));
      }
      return;
    case IrStmt::Kind::Assign:
      line(std::format("{} = {};", exprToWgsl(data.exprs[0]), exprToWgsl(data.exprs[1])));
      return;
    case IrStmt::Kind::If:
      line(std::format("if ({}) {{", exprToWgsl(data.exprs[0])));
      ++indent_;
      emitBlock(data.body, function);
      --indent_;
      if (!data.elseBody.empty()) {
        line("} else {");
        ++indent_;
        emitBlock(data.elseBody, function);
        --indent_;
      }
      line("}");
      return;
    case IrStmt::Kind::For: {
      std::string header = "for (";
      if (data.init) {
        const IrStmt::Data& init = data.init->data();
        check(CheckIdentifier(init.name, "for init"));
        header += std::format("var {}: {} = {}", init.name.str(), TypeToWgsl(*init.declaredType),
                              exprToWgsl(init.exprs[0]));
      }
      header += "; ";
      if (!data.exprs.empty()) {
        header += exprToWgsl(data.exprs[0]);
      }
      header += "; ";
      if (data.continuing) {
        const IrStmt::Data& continuing = data.continuing->data();
        header += std::format("{} = {}", exprToWgsl(continuing.exprs[0]),
                              exprToWgsl(continuing.exprs[1]));
      }
      header += ") {";
      line(std::move(header));
      ++indent_;
      emitBlock(data.body, function);
      --indent_;
      line("}");
      return;
    }
    case IrStmt::Kind::Break: line("break;"); return;
    case IrStmt::Kind::Continue: line("continue;"); return;
    case IrStmt::Kind::Return: {
      if (function.stage != StageKind::None) {
        std::string result = std::format("return {}(", OutputStructName(function));
        for (size_t i = 0; i < data.exprs.size(); ++i) {
          if (i > 0) {
            result += ", ";
          }
          result += exprToWgsl(data.exprs[i]);
        }
        result += ");";
        line(std::move(result));
      } else if (!data.exprs.empty()) {
        line(std::format("return {};", exprToWgsl(data.exprs[0])));
      } else {
        line("return;");
      }
      return;
    }
    case IrStmt::Kind::Discard: line("discard;"); return;
  }
}

void Emitter::emitBlock(const IrBlock& block, const IrFunction& function) {
  for (const IrStmt& statement : block) {
    emitStatement(statement, function);
  }
}

void Emitter::collectStructs(const IrType& type, std::vector<IrType>& out) {
  switch (type.kind()) {
    case IrType::Kind::Struct: {
      for (const IrType& existing : out) {
        if (existing.structName() == type.structName()) {
          if (!(existing == type)) {
            latch(ShaderError{
                std::format("two distinct struct types share the name {}", type.structName().str()),
                "wgsl"});
          }
          return;
        }
      }
      // Dependencies (member structs) come first so definitions precede uses.
      for (const IrType::Member& member : type.structMembers()) {
        collectStructs(member.type, out);
      }
      out.push_back(type);
      return;
    }
    case IrType::Kind::SizedArray:
    case IrType::Kind::RuntimeArray: collectStructs(type.elementType(), out); return;
    default: return;
  }
}

void Emitter::checkUniformArrays(const IrType& type) {
  switch (type.kind()) {
    case IrType::Kind::SizedArray:
    case IrType::Kind::RuntimeArray: {
      ShaderResult<ArrayStrideInfo> info = ComputeArrayStrideInfo(type, AddressSpace::Uniform);
      if (info.hasError()) {
        latch(std::move(info).error());
        return;
      }
      if (info.result().paddedFromNatural) {
        latch(ShaderError{
            std::format("uniform array {} requires a padded element wrapper (natural stride is "
                        "not a multiple of 16); the emitter does not materialize wrappers yet",
                        TypeToWgsl(type)),
            "wgsl"});
        return;
      }
      checkUniformArrays(type.elementType());
      return;
    }
    case IrType::Kind::Struct:
      for (const IrType::Member& member : type.structMembers()) {
        checkUniformArrays(member.type);
      }
      return;
    default: return;
  }
}

void Emitter::validateBindingLayout(const IrType& type, AddressSpace addressSpace) {
  // Route the full binding root type through the layout engine so non-host-shareable contents
  // (bool members, resource types) fail closed instead of emitting silently.
  switch (type.kind()) {
    case IrType::Kind::Struct: {
      if (ShaderResult<StructLayout> layout = ComputeStructLayout(type, addressSpace);
          layout.hasError()) {
        latch(std::move(layout).error());
      }
      return;
    }
    case IrType::Kind::RuntimeArray: {
      if (ShaderResult<uint32_t> stride = ComputeArrayStride(type, addressSpace);
          stride.hasError()) {
        latch(std::move(stride).error());
        return;
      }
      if (type.elementType().kind() == IrType::Kind::Struct) {
        validateBindingLayout(type.elementType(), addressSpace);
      }
      return;
    }
    default: {
      if (ShaderResult<TypeLayout> layout = ComputeTypeLayout(type, addressSpace);
          layout.hasError()) {
        latch(std::move(layout).error());
      }
      return;
    }
  }
}

void Emitter::emitStructDeclarations() {
  std::vector<IrType> structs;
  for (const IrBinding& binding : module_.bindings()) {
    collectStructs(binding.type, structs);
  }
  for (const IrFunction& function : module_.functions()) {
    for (const IrParam& param : function.params) {
      collectStructs(param.type, structs);
    }
    if (function.returnType) {
      collectStructs(*function.returnType, structs);
    }
    for (const IrOutputMember& output : function.outputs) {
      collectStructs(output.type, structs);
    }
    // Function-scope var declarations may introduce struct types not visible in any signature;
    // for-loop init variables live in IrStmt::Data::init rather than the body block, so they
    // are collected explicitly.
    const std::function<void(const IrBlock&)> walkBlock = [&](const IrBlock& block) {
      for (const IrStmt& statement : block) {
        const IrStmt::Data& data = statement.data();
        if (statement.kind() == IrStmt::Kind::Var && data.declaredType) {
          collectStructs(*data.declaredType, structs);
        }
        if (data.init && data.init->data().declaredType) {
          collectStructs(*data.init->data().declaredType, structs);
        }
        walkBlock(data.body);
        walkBlock(data.elseBody);
      }
    };
    walkBlock(function.body);
  }

  for (const IrType& structType : structs) {
    userStructNames_.push_back(structType.structName());
    check(CheckIdentifier(structType.structName(), "struct"));
    line(std::format("struct {} {{", structType.structName().str()));
    ++indent_;
    for (const IrType::Member& member : structType.structMembers()) {
      check(CheckIdentifier(member.name, "struct member"));
      line(std::format("{}: {},", member.name.str(), TypeToWgsl(member.type)));
    }
    --indent_;
    line("}");
    blank();
  }
}

void Emitter::emitConstants() {
  for (const IrConstant& constant : module_.constants()) {
    check(CheckIdentifier(constant.name, "const"));
    line(std::format("const {}: {} = {};", constant.name.str(), TypeToWgsl(constant.value.type()),
                     exprToWgsl(constant.value)));
  }
  if (!module_.constants().empty()) {
    blank();
  }
}

void Emitter::emitBindings() {
  for (const IrBinding& binding : module_.bindings()) {
    check(CheckIdentifier(binding.name, "binding"));
    std::string declaration;
    switch (binding.kind) {
      case BindingKind::UniformBuffer:
        validateBindingLayout(binding.type, AddressSpace::Uniform);
        checkUniformArrays(binding.type);
        declaration =
            std::format("var<uniform> {}: {};", binding.name.str(), TypeToWgsl(binding.type));
        break;
      case BindingKind::ReadOnlyStorageBuffer:
        validateBindingLayout(binding.type, AddressSpace::Storage);
        declaration =
            std::format("var<storage, read> {}: {};", binding.name.str(), TypeToWgsl(binding.type));
        break;
      case BindingKind::SampledTexture2dF32:
        declaration = std::format("var {}: texture_2d<f32>;", binding.name.str());
        break;
      case BindingKind::FilteringSampler:
        declaration = std::format("var {}: sampler;", binding.name.str());
        break;
    }
    line(std::format("@group({}) @binding({}) {}", binding.group, binding.binding, declaration));
  }
  if (!module_.bindings().empty()) {
    blank();
  }
}

void Emitter::emitFunction(const IrFunction& function) {
  check(CheckIdentifier(function.name, "function"));

  if (function.stage != StageKind::None) {
    // The generated output struct name must not collide with a user struct. Detected here (not
    // reserved at builder registration) so the target-neutral IR carries no emitter naming
    // rules; the emitter fails closed instead of declaring two conflicting structs.
    for (const RcString& userStruct : userStructNames_) {
      if (std::string_view(userStruct) == OutputStructName(function)) {
        latch(ShaderError{
            std::format("struct \"{}\" collides with the generated output struct name for "
                        "entry point {}",
                        userStruct.str(), std::string_view(function.name)),
            "wgsl"});
      }
    }

    // Generated output struct with annotated members.
    line(std::format("struct {} {{", OutputStructName(function)));
    ++indent_;
    for (const IrOutputMember& output : function.outputs) {
      check(CheckIdentifier(output.name, "entry point output"));
      std::string annotation;
      if (output.builtin) {
        annotation = "@builtin(position) ";
      } else {
        annotation = std::format("@location({}) ", *output.location);
      }
      line(std::format("{}{}: {},", annotation, output.name.str(), TypeToWgsl(output.type)));
    }
    --indent_;
    line("}");
    blank();
    line(function.stage == StageKind::Vertex ? "@vertex" : "@fragment");
  }

  std::string signature = std::format("fn {}(", std::string_view(function.name));
  for (size_t i = 0; i < function.params.size(); ++i) {
    const IrParam& param = function.params[i];
    check(CheckIdentifier(param.name, "parameter"));
    if (i > 0) {
      signature += ", ";
    }
    if (param.builtin) {
      signature += *param.builtin == BuiltinInput::InstanceIndex ? "@builtin(instance_index) "
                                                                 : "@builtin(position) ";
    } else if (param.location) {
      signature += std::format("@location({}) ", *param.location);
    }
    signature += std::format("{}: {}", param.name.str(), TypeToWgsl(param.type));
  }
  signature += ")";
  if (function.stage != StageKind::None) {
    signature += std::format(" -> {}", OutputStructName(function));
  } else if (function.returnType) {
    signature += std::format(" -> {}", TypeToWgsl(*function.returnType));
  }
  signature += " {";
  line(std::move(signature));

  ++indent_;
  emitBlock(function.body, function);
  --indent_;
  line("}");
  blank();
}

ShaderResult<std::string> Emitter::emit() {
  out_ += "// Generated by donner::gpu::shader::WgslEmitter. Do not edit.\n\n";

  emitStructDeclarations();
  emitConstants();
  emitBindings();
  for (const IrFunction& function : module_.functions()) {
    emitFunction(function);
  }

  if (error_) {
    return *error_;
  }

  // Normalize the tail: exactly one trailing newline.
  while (out_.size() >= 2 && out_[out_.size() - 1] == '\n' && out_[out_.size() - 2] == '\n') {
    out_.pop_back();
  }
  return std::move(out_);
}

}  // namespace

ShaderResult<std::string> EmitWgsl(const IrModule& module) {
  return Emitter(module).emit();
}

}  // namespace donner::gpu::shader
