#include "donner/gpu/shader/MslEmitter.h"

#include <cmath>
#include <format>
#include <functional>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "donner/gpu/shader/IrLayout.h"
#include "donner/gpu/shader/MslBindingMap.h"

namespace donner::gpu::shader {

namespace {

/// C++/MSL keywords and type names that IR identifiers must not collide with. Conservative
/// subset covering the C++14 keyword table plus the metal namespace types this emitter spells
/// unqualified.
constexpr std::string_view kMslReservedWords[] = {
    "alignas", "alignof",  "auto",        "bool",      "break",    "case",      "catch",
    "char",    "class",    "const",       "constexpr", "continue", "default",   "delete",
    "device",  "do",       "double",      "else",      "enum",     "explicit",  "extern",
    "false",   "float",    "for",         "fragment",  "friend",   "goto",      "half",
    "if",      "inline",   "int",         "kernel",    "long",     "mutable",   "namespace",
    "new",     "operator", "private",     "protected", "public",   "register",  "return",
    "short",   "signed",   "sizeof",      "static",    "struct",   "switch",    "template",
    "this",    "thread",   "threadgroup", "throw",     "true",     "try",       "typedef",
    "typeid",  "typename", "uint",        "union",     "unsigned", "using",     "vertex",
    "virtual", "void",     "volatile",    "while",     "constant", "float2",    "float3",
    "float4",  "float4x4", "int2",        "int3",      "int4",     "uint2",     "uint3",
    "uint4",   "bool2",    "bool3",       "bool4",     "sampler",  "texture2d", "array",
    "metal",   "select",   "saturate",    "fract",     "fwidth",   "clamp",     "abs",
    "min",     "max",      "sqrt",        "length",    "round",
};

/// Checks an identifier for C++/MSL lexical validity and reserved-word collisions; fails
/// closed. As with the WGSL emitter, lexical enforcement lives here rather than in the
/// target-neutral module builder.
ShaderStatus CheckMslIdentifier(const RcString& name, std::string_view context) {
  if (name.empty()) {
    return ShaderError{std::format("{}: identifier is empty", context), "msl"};
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
  // C++ reserves identifiers containing a double underscore; reject the leading form the same
  // way the WGSL emitter does.
  if (!lexicallyValid || text.starts_with("__")) {
    return ShaderError{
        std::format("{}: \"{}\" is not a valid identifier (MSL identifiers match "
                    "[A-Za-z_][A-Za-z0-9_]* and may not start with a double underscore)",
                    context, name.str()),
        "msl"};
  }

  for (const std::string_view reserved : kMslReservedWords) {
    if (std::string_view(name) == reserved) {
      return ShaderError{std::format("{}: identifier \"{}\" collides with an MSL reserved word",
                                     context, name.str()),
                         "msl"};
    }
  }
  return OkShaderStatus();
}

/// MSL spelling of a type, e.g. `float2` or `Band`.
std::string TypeToMsl(const IrType& type) {
  switch (type.kind()) {
    case IrType::Kind::Scalar:
      switch (type.scalarKind()) {
        case ScalarKind::Bool: return "bool";
        case ScalarKind::I32: return "int";
        case ScalarKind::U32: return "uint";
        case ScalarKind::F32: return "float";
      }
      return "float";
    case IrType::Kind::Vector: {
      std::string element;
      switch (type.scalarKind()) {
        case ScalarKind::Bool: element = "bool"; break;
        case ScalarKind::I32: element = "int"; break;
        case ScalarKind::U32: element = "uint"; break;
        case ScalarKind::F32: element = "float"; break;
      }
      return std::format("{}{}", element, type.vectorSize());
    }
    case IrType::Kind::Matrix4x4f: return "float4x4";
    case IrType::Kind::SizedArray:
      // Only valid as a struct member; spelled as a C array at the member site.
      return TypeToMsl(type.elementType());
    case IrType::Kind::RuntimeArray: return TypeToMsl(type.elementType());
    case IrType::Kind::Struct: return type.structName().str();
    case IrType::Kind::Texture2dF32: return "texture2d<float>";
    case IrType::Kind::Sampler: return "sampler";
  }
  return "float";
}

/// Rounds \p value up to the next multiple of \p alignment.
uint32_t RoundUp(uint32_t alignment, uint32_t value) {
  return ((value + alignment - 1) / alignment) * alignment;
}

/// MSL natural alignment and size of a host-shareable type. Returns nullopt for types with no
/// MSL buffer layout in this emitter's scope.
struct MslLayout {
  uint32_t alignBytes = 0;  //!< Natural MSL alignment.
  uint32_t sizeBytes = 0;   //!< Natural MSL size (sizeof).
};
std::optional<MslLayout> ComputeMslLayout(const IrType& type) {
  switch (type.kind()) {
    case IrType::Kind::Scalar:
      if (type.scalarKind() == ScalarKind::Bool) {
        return std::nullopt;
      }
      return MslLayout{4, 4};
    case IrType::Kind::Vector: {
      if (type.scalarKind() == ScalarKind::Bool) {
        return std::nullopt;
      }
      switch (type.vectorSize()) {
        case 2: return MslLayout{8, 8};
        // MSL float3 is 16 bytes (unlike WGSL's 12-byte vec3); the divergence is caught by the
        // member-by-member comparison against the WGSL layout engine.
        case 3: return MslLayout{16, 16};
        case 4: return MslLayout{16, 16};
        default: return std::nullopt;
      }
    }
    case IrType::Kind::Matrix4x4f: return MslLayout{16, 64};
    case IrType::Kind::SizedArray: {
      const std::optional<MslLayout> element = ComputeMslLayout(type.elementType());
      if (!element) {
        return std::nullopt;
      }
      const uint32_t stride = RoundUp(element->alignBytes, element->sizeBytes);
      return MslLayout{element->alignBytes, stride * type.arrayCount()};
    }
    case IrType::Kind::Struct: {
      uint32_t offset = 0;
      uint32_t align = 0;
      for (const IrType::Member& member : type.structMembers()) {
        const std::optional<MslLayout> memberLayout = ComputeMslLayout(member.type);
        if (!memberLayout) {
          return std::nullopt;
        }
        offset = RoundUp(memberLayout->alignBytes, offset) + memberLayout->sizeBytes;
        align = std::max(align, memberLayout->alignBytes);
      }
      return MslLayout{align, RoundUp(align, offset)};
    }
    default: return std::nullopt;
  }
}

/// Computes the MSL natural offset of every member of \p structType (mirroring what the Metal
/// compiler lays out for a plain C++ struct of the mapped types).
std::optional<std::vector<uint32_t>> ComputeMslMemberOffsets(const IrType& structType) {
  std::vector<uint32_t> offsets;
  uint32_t offset = 0;
  for (const IrType::Member& member : structType.structMembers()) {
    const std::optional<MslLayout> memberLayout = ComputeMslLayout(member.type);
    if (!memberLayout) {
      return std::nullopt;
    }
    offset = RoundUp(memberLayout->alignBytes, offset);
    offsets.push_back(offset);
    offset += memberLayout->sizeBytes;
  }
  return offsets;
}

/// Emitter state: output text plus the first latched error.
class Emitter {
public:
  explicit Emitter(const IrModule& module) : module_(module) {}

  /// Runs emission. @return The MSL text or the first error.
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

  std::string literalToMsl(const IrExpr::Node& node);
  std::string exprToMsl(const IrExpr& expr);
  void emitStatement(const IrStmt& statement, const IrFunction& function);
  void emitBlock(const IrBlock& block, const IrFunction& function);

  void collectStructs(const IrType& type, std::vector<IrType>& out);
  void verifyBufferStructLayouts(const IrType& type, AddressSpace addressSpace);
  void emitStructDeclarations();
  void emitConstants();
  void emitFunction(const IrFunction& function);

  /// Binding parameter spelling for plain-function parameter lists (no attributes).
  std::string bindingParam(const IrBinding& binding) const;
  /// Binding parameter spelling for entry point signatures (with argument-table attributes).
  std::string bindingEntryParam(const IrBinding& binding) const;
  /// Comma-joined binding names, used to forward bindings through user-function calls.
  std::string bindingForwardArgs() const;

  /// Generated IO struct names for an entry point.
  static std::string InputStructName(const IrFunction& function) {
    return std::format("{}_Input", std::string_view(function.name));
  }
  static std::string OutputStructName(const IrFunction& function) {
    return std::format("{}_Output", std::string_view(function.name));
  }

  /// Rejects user names that would shadow an implicit MSL identifier: binding parameter names
  /// (present on every function) or, inside entry points, the generated stage-in parameter
  /// `in`. Shadowing would be legal C++ but wrong-meaning MSL - user-call forwarding passes
  /// binding names, which would resolve to the shadowing local.
  ShaderStatus checkNoImplicitShadow(const RcString& name, std::string_view context,
                                     bool insideEntryPoint) const {
    for (const IrBinding& binding : module_.bindings()) {
      if (name == binding.name) {
        return ShaderError{std::format("{}: \"{}\" shadows an implicit MSL binding parameter",
                                       context, name.str()),
                           "msl"};
      }
    }
    if (insideEntryPoint && std::string_view(name) == "in") {
      return ShaderError{
          std::format("{}: \"{}\" shadows an implicit MSL stage-in parameter", context, name.str()),
          "msl"};
    }
    return OkShaderStatus();
  }

  const IrModule& module_;
  std::string out_;
  int indent_ = 0;
  std::vector<RcString> userStructNames_;
  std::optional<ShaderError> error_;
};

std::string Emitter::literalToMsl(const IrExpr::Node& node) {
  if (std::holds_alternative<bool>(node.literal)) {
    return std::get<bool>(node.literal) ? "true" : "false";
  }
  if (std::holds_alternative<int32_t>(node.literal)) {
    return std::format("{}", std::get<int32_t>(node.literal));
  }
  if (std::holds_alternative<uint32_t>(node.literal)) {
    return std::format("{}u", std::get<uint32_t>(node.literal));
  }

  const float value = std::get<float>(node.literal);
  if (!std::isfinite(value)) {
    latch(ShaderError{"non-finite float literals cannot be emitted as MSL", "msl"});
    return "0.0f";
  }
  // Shortest round-trip form, made C++-legal: a float literal needs a decimal point or an
  // exponent before the `f` suffix.
  std::string text = std::format("{}", value);
  if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
      text.find('E') == std::string::npos && text.find("inf") == std::string::npos) {
    text += ".0";
  }
  return text + "f";
}

std::string Emitter::exprToMsl(const IrExpr& expr) {
  const IrExpr::Node& node = expr.node();
  switch (node.kind) {
    case IrExpr::Kind::Literal: return literalToMsl(node);
    case IrExpr::Kind::Ref: return node.name.str();
    case IrExpr::Kind::Unary:
      return std::format("({}{})", node.unaryOp == IrExpr::UnaryOp::Neg ? "-" : "!",
                         exprToMsl(node.children[0]));
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
      return std::format("({} {} {})", exprToMsl(node.children[0]), op,
                         exprToMsl(node.children[1]));
    }
    case IrExpr::Kind::Member:
      return std::format("{}.{}", exprToMsl(node.children[0]), node.name.str());
    case IrExpr::Kind::Swizzle:
      return std::format("{}.{}", exprToMsl(node.children[0]), node.swizzle);
    case IrExpr::Kind::Index:
      return std::format("{}[{}]", exprToMsl(node.children[0]), exprToMsl(node.children[1]));
    case IrExpr::Kind::Construct:
    case IrExpr::Kind::Convert: {
      std::string result = TypeToMsl(node.type);
      result += "(";
      for (size_t i = 0; i < node.children.size(); ++i) {
        if (i > 0) {
          result += ", ";
        }
        result += exprToMsl(node.children[i]);
      }
      result += ")";
      return result;
    }
    case IrExpr::Kind::CallBuiltin: {
      switch (node.builtin) {
        case BuiltinFn::Select:
          // WGSL select(falseValue, trueValue, condition).
          return std::format("({} ? {} : {})", exprToMsl(node.children[2]),
                             exprToMsl(node.children[1]), exprToMsl(node.children[0]));
        case BuiltinFn::TextureSample:
          return std::format("{}.sample({}, {})", exprToMsl(node.children[0]),
                             exprToMsl(node.children[1]), exprToMsl(node.children[2]));
        case BuiltinFn::TextureLoad:
          return std::format("{}.read(uint2({}), uint({}))", exprToMsl(node.children[0]),
                             exprToMsl(node.children[1]), exprToMsl(node.children[2]));
        case BuiltinFn::TextureDimensions:
          return std::format("uint2({0}.get_width(), {0}.get_height())",
                             exprToMsl(node.children[0]));
        default: break;
      }

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
        default: name = "abs"; break;
      }
      std::string result = name + "(";
      for (size_t i = 0; i < node.children.size(); ++i) {
        if (i > 0) {
          result += ", ";
        }
        result += exprToMsl(node.children[i]);
      }
      result += ")";
      return result;
    }
    case IrExpr::Kind::CallUser: {
      // User calls forward every module binding ahead of the declared arguments.
      std::string result = node.name.str() + "(";
      result += bindingForwardArgs();
      for (const IrExpr& child : node.children) {
        if (!result.ends_with("(")) {
          result += ", ";
        }
        result += exprToMsl(child);
      }
      result += ")";
      return result;
    }
  }
  return "0.0f";
}

void Emitter::emitStatement(const IrStmt& statement, const IrFunction& function) {
  const IrStmt::Data& data = statement.data();
  switch (statement.kind()) {
    case IrStmt::Kind::Let:
      check(CheckMslIdentifier(data.name, "let"));
      check(checkNoImplicitShadow(data.name, "let", function.stage != StageKind::None));
      line(std::format("const {} {} = {};", TypeToMsl(data.exprs[0].type()), data.name.str(),
                       exprToMsl(data.exprs[0])));
      return;
    case IrStmt::Kind::Var:
      check(CheckMslIdentifier(data.name, "var"));
      check(checkNoImplicitShadow(data.name, "var", function.stage != StageKind::None));
      if (!data.exprs.empty()) {
        line(std::format("{} {} = {};", TypeToMsl(*data.declaredType), data.name.str(),
                         exprToMsl(data.exprs[0])));
      } else {
        line(std::format("{} {};", TypeToMsl(*data.declaredType), data.name.str()));
      }
      return;
    case IrStmt::Kind::Assign:
      line(std::format("{} = {};", exprToMsl(data.exprs[0]), exprToMsl(data.exprs[1])));
      return;
    case IrStmt::Kind::If:
      line(std::format("if ({}) {{", exprToMsl(data.exprs[0])));
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
        check(CheckMslIdentifier(init.name, "for init"));
        check(checkNoImplicitShadow(init.name, "for init", function.stage != StageKind::None));
        header += std::format("{} {} = {}", TypeToMsl(*init.declaredType), init.name.str(),
                              exprToMsl(init.exprs[0]));
      }
      header += "; ";
      if (!data.exprs.empty()) {
        header += exprToMsl(data.exprs[0]);
      }
      header += "; ";
      if (data.continuing) {
        const IrStmt::Data& continuing = data.continuing->data();
        header +=
            std::format("{} = {}", exprToMsl(continuing.exprs[0]), exprToMsl(continuing.exprs[1]));
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
        std::string result = std::format("return {}{{", OutputStructName(function));
        for (size_t i = 0; i < data.exprs.size(); ++i) {
          if (i > 0) {
            result += ", ";
          }
          result += exprToMsl(data.exprs[i]);
        }
        result += "};";
        line(std::move(result));
      } else if (!data.exprs.empty()) {
        line(std::format("return {};", exprToMsl(data.exprs[0])));
      } else {
        line("return;");
      }
      return;
    }
    case IrStmt::Kind::Discard: line("discard_fragment();"); return;
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
                "msl"});
          }
          return;
        }
      }
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

void Emitter::verifyBufferStructLayouts(const IrType& type, AddressSpace addressSpace) {
  switch (type.kind()) {
    case IrType::Kind::Struct: {
      ShaderResult<StructLayout> wgslLayout = ComputeStructLayout(type, addressSpace);
      if (wgslLayout.hasError()) {
        latch(std::move(wgslLayout).error());
        return;
      }
      const std::optional<std::vector<uint32_t>> mslOffsets = ComputeMslMemberOffsets(type);
      const std::optional<MslLayout> mslLayout = ComputeMslLayout(type);
      if (!mslOffsets || !mslLayout) {
        latch(ShaderError{
            std::format("struct {} has no MSL buffer layout", type.structName().str()), "msl"});
        return;
      }
      const std::span<const IrType::Member> members = type.structMembers();
      for (size_t i = 0; i < members.size(); ++i) {
        if ((*mslOffsets)[i] != wgslLayout.result().members[i].offsetBytes) {
          latch(ShaderError{
              std::format("struct {} member {}: MSL natural offset {} diverges from the WGSL "
                          "layout offset {}; the Metal path cannot share this layout",
                          type.structName().str(), members[i].name.str(), (*mslOffsets)[i],
                          wgslLayout.result().members[i].offsetBytes),
              "msl"});
          return;
        }
      }
      if (mslLayout->sizeBytes != wgslLayout.result().sizeBytes) {
        latch(ShaderError{
            std::format("struct {}: MSL natural size {} diverges from the WGSL layout size {}",
                        type.structName().str(), mslLayout->sizeBytes,
                        wgslLayout.result().sizeBytes),
            "msl"});
        return;
      }
      for (const IrType::Member& member : members) {
        verifyBufferStructLayouts(member.type, addressSpace);
      }
      return;
    }
    case IrType::Kind::SizedArray:
    case IrType::Kind::RuntimeArray: {
      // Array strides must also agree (the WGSL uniform 16-byte rounding has no MSL C-array
      // equivalent without a wrapper).
      ShaderResult<uint32_t> wgslStride = ComputeArrayStride(type, addressSpace);
      const std::optional<MslLayout> element = ComputeMslLayout(type.elementType());
      if (wgslStride.hasError() || !element) {
        latch(ShaderError{"array element has no shared MSL/WGSL layout", "msl"});
        return;
      }
      const uint32_t mslStride = RoundUp(element->alignBytes, element->sizeBytes);
      if (mslStride != wgslStride.result()) {
        latch(ShaderError{
            std::format("array stride diverges between MSL ({}) and the WGSL layout ({}); a "
                        "padded element wrapper would be required",
                        mslStride, wgslStride.result()),
            "msl"});
        return;
      }
      verifyBufferStructLayouts(type.elementType(), addressSpace);
      return;
    }
    default: return;
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
    check(CheckMslIdentifier(structType.structName(), "struct"));
    line(std::format("struct {} {{", structType.structName().str()));
    ++indent_;
    for (const IrType::Member& member : structType.structMembers()) {
      check(CheckMslIdentifier(member.name, "struct member"));
      if (member.type.kind() == IrType::Kind::SizedArray) {
        line(std::format("{} {}[{}];", TypeToMsl(member.type.elementType()), member.name.str(),
                         member.type.arrayCount()));
      } else {
        line(std::format("{} {};", TypeToMsl(member.type), member.name.str()));
      }
    }
    --indent_;
    line("};");
    blank();
  }
}

void Emitter::emitConstants() {
  for (const IrConstant& constant : module_.constants()) {
    check(CheckMslIdentifier(constant.name, "const"));
    line(std::format("constant {} {} = {};", TypeToMsl(constant.value.type()), constant.name.str(),
                     exprToMsl(constant.value)));
  }
  if (!module_.constants().empty()) {
    blank();
  }
}

std::string Emitter::bindingParam(const IrBinding& binding) const {
  switch (binding.kind) {
    case BindingKind::UniformBuffer:
      return std::format("constant {}& {}", TypeToMsl(binding.type), binding.name.str());
    case BindingKind::ReadOnlyStorageBuffer:
      if (binding.type.kind() == IrType::Kind::RuntimeArray) {
        return std::format("device const {}* {}", TypeToMsl(binding.type.elementType()),
                           binding.name.str());
      }
      return std::format("device const {}& {}", TypeToMsl(binding.type), binding.name.str());
    case BindingKind::SampledTexture2dF32:
      return std::format("texture2d<float> {}", binding.name.str());
    case BindingKind::FilteringSampler: return std::format("sampler {}", binding.name.str());
  }
  return "";
}

std::string Emitter::bindingEntryParam(const IrBinding& binding) const {
  switch (binding.kind) {
    case BindingKind::UniformBuffer:
    case BindingKind::ReadOnlyStorageBuffer:
      return std::format("{} [[buffer({})]]", bindingParam(binding),
                         MslBufferIndex(binding.binding));
    case BindingKind::SampledTexture2dF32:
      return std::format("{} [[texture({})]]", bindingParam(binding),
                         MslTextureIndex(binding.binding));
    case BindingKind::FilteringSampler:
      return std::format("{} [[sampler({})]]", bindingParam(binding),
                         MslSamplerIndex(binding.binding));
  }
  return "";
}

std::string Emitter::bindingForwardArgs() const {
  std::string result;
  for (const IrBinding& binding : module_.bindings()) {
    if (!result.empty()) {
      result += ", ";
    }
    result += binding.name.str();
  }
  return result;
}

void Emitter::emitFunction(const IrFunction& function) {
  check(CheckMslIdentifier(function.name, "function"));

  if (function.stage != StageKind::None) {
    // The generated stage IO struct names must not collide with user structs; fail closed
    // rather than declaring conflicting types (mirrors the WGSL emitter's check).
    for (const RcString& userStruct : userStructNames_) {
      if (std::string_view(userStruct) == InputStructName(function) ||
          std::string_view(userStruct) == OutputStructName(function)) {
        latch(ShaderError{
            std::format("struct \"{}\" collides with a generated stage IO struct name for "
                        "entry point {}",
                        userStruct.str(), std::string_view(function.name)),
            "msl"});
      }
    }

    // Generated stage-in struct: location params become [[attribute(N)]] (vertex) or
    // [[user(locnN)]] (fragment); the fragment position builtin is a [[position]] member.
    line(std::format("struct {} {{", InputStructName(function)));
    ++indent_;
    for (const IrParam& param : function.params) {
      check(CheckMslIdentifier(param.name, "entry point input"));
      if (param.builtin && *param.builtin == BuiltinInput::Position) {
        line(std::format("{} {} [[position]];", TypeToMsl(param.type), param.name.str()));
      } else if (param.location) {
        const std::string annotation = function.stage == StageKind::Vertex
                                           ? std::format("[[attribute({})]]", *param.location)
                                           : std::format("[[user(locn{})]]", *param.location);
        line(std::format("{} {} {};", TypeToMsl(param.type), param.name.str(), annotation));
      }
      // The instance_index builtin is a direct entry point parameter, not a stage-in field.
    }
    --indent_;
    line("};");
    blank();

    line(std::format("struct {} {{", OutputStructName(function)));
    ++indent_;
    for (const IrOutputMember& output : function.outputs) {
      check(CheckMslIdentifier(output.name, "entry point output"));
      std::string annotation;
      if (output.builtin) {
        annotation = "[[position]]";
      } else if (function.stage == StageKind::Vertex) {
        annotation = std::format("[[user(locn{})]]", *output.location);
      } else {
        annotation = std::format("[[color({})]]", *output.location);
      }
      line(std::format("{} {} {};", TypeToMsl(output.type), output.name.str(), annotation));
    }
    --indent_;
    line("};");
    blank();
  }

  // Signature.
  std::string signature;
  if (function.stage == StageKind::Vertex) {
    signature =
        std::format("vertex {} {}(", OutputStructName(function), std::string_view(function.name));
  } else if (function.stage == StageKind::Fragment) {
    signature =
        std::format("fragment {} {}(", OutputStructName(function), std::string_view(function.name));
  } else {
    signature =
        std::format("{} {}(", function.returnType ? TypeToMsl(*function.returnType) : "void",
                    std::string_view(function.name));
  }

  bool firstParam = true;
  const auto addParam = [&](std::string text) {
    if (!firstParam) {
      signature += ", ";
    }
    signature += text;
    firstParam = false;
  };

  if (function.stage != StageKind::None) {
    addParam(std::format("{} in [[stage_in]]", InputStructName(function)));
    for (const IrParam& param : function.params) {
      if (param.builtin && *param.builtin == BuiltinInput::InstanceIndex) {
        addParam(std::format("uint {} [[instance_id]]", param.name.str()));
      }
    }
    // Every module binding is declared on every entry point; a declared-but-unbound argument
    // slot is legal only while the stage genuinely never references it, which holds because
    // the Metal backend binds every group entry for each stage its layout declares.
    for (const IrBinding& binding : module_.bindings()) {
      addParam(bindingEntryParam(binding));
    }
  } else {
    for (const IrBinding& binding : module_.bindings()) {
      addParam(bindingParam(binding));
    }
    for (const IrParam& param : function.params) {
      check(CheckMslIdentifier(param.name, "parameter"));
      check(checkNoImplicitShadow(param.name, "parameter", /*insideEntryPoint=*/false));
      addParam(std::format("{} {}", TypeToMsl(param.type), param.name.str()));
    }
  }
  signature += ") {";
  line(std::move(signature));

  ++indent_;
  if (function.stage != StageKind::None) {
    // Alias stage-in fields to their IR names so references emit unchanged.
    for (const IrParam& param : function.params) {
      check(checkNoImplicitShadow(param.name, "entry point input",
                                  /*insideEntryPoint=*/true));
      if (param.builtin && *param.builtin == BuiltinInput::InstanceIndex) {
        continue;  // Already a direct parameter.
      }
      line(std::format("const {} {} = in.{};", TypeToMsl(param.type), param.name.str(),
                       param.name.str()));
    }
  }
  emitBlock(function.body, function);
  --indent_;
  line("}");
  blank();
}

ShaderResult<std::string> Emitter::emit() {
  out_ += "// Generated by donner::gpu::shader::MslEmitter. Do not edit.\n\n";
  out_ += "#include <metal_stdlib>\n\nusing namespace metal;\n\n";

  // Verify every buffer-referenced struct's MSL natural layout matches the WGSL layout engine
  // before emitting anything that depends on it.
  for (const IrBinding& binding : module_.bindings()) {
    switch (binding.kind) {
      case BindingKind::UniformBuffer:
        verifyBufferStructLayouts(binding.type, AddressSpace::Uniform);
        break;
      case BindingKind::ReadOnlyStorageBuffer:
        verifyBufferStructLayouts(binding.type, AddressSpace::Storage);
        break;
      default: break;
    }
    // The flat Metal argument-table map models only bind group 0 today (the solid-fill family
    // is single-group); multi-group support arrives with later pipeline families.
    if (binding.group != 0) {
      latch(ShaderError{
          std::format("binding {} is in group {}; the Metal binding map models only bind group "
                      "0 today",
                      binding.binding, binding.group),
          "msl"});
    }
    if ((binding.kind == BindingKind::UniformBuffer ||
         binding.kind == BindingKind::ReadOnlyStorageBuffer) &&
        MslBufferIndex(binding.binding) >= kMslVertexBufferIndex) {
      latch(ShaderError{
          std::format("buffer binding {} maps to Metal buffer index {}, which collides with or "
                      "exceeds the reserved stage-in vertex buffer index {}",
                      binding.binding, MslBufferIndex(binding.binding), kMslVertexBufferIndex),
          "msl"});
    }
    check(CheckMslIdentifier(binding.name, "binding"));
  }

  emitStructDeclarations();
  emitConstants();
  for (const IrFunction& function : module_.functions()) {
    emitFunction(function);
  }

  if (error_) {
    return *error_;
  }

  while (out_.size() >= 2 && out_[out_.size() - 1] == '\n' && out_[out_.size() - 2] == '\n') {
    out_.pop_back();
  }
  return std::move(out_);
}

}  // namespace

ShaderResult<std::string> EmitMsl(const IrModule& module) {
  return Emitter(module).emit();
}

}  // namespace donner::gpu::shader
