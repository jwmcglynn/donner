#pragma once
/// @file
/// Constexpr-capable text emission for the bounded WGSL module profile.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "donner/gpu/shader/MslBindingMap.h"
#include "donner/gpu/shader/wgsl/Module.h"

namespace donner::gpu::shader::wgsl {

/// An error produced while projecting a validated module into text.
enum class TextEmitError : uint8_t {
  None,
  InvalidModule,
  SinkTooSmall,
  InvalidArenaReference,
  UnsupportedType,
  UnsupportedBinding,
  UnsupportedEntryPointName,
  UniformLayoutMismatch,
};

/// The result of bounded text emission.
struct TextEmitResult {
  TextEmitError error = TextEmitError::None;

  /// Returns whether the full text was written.
  constexpr bool ok() const { return error == TextEmitError::None; }
};

/// Maximum bytes emitted for one text projection, including count-only calls.
inline constexpr uint32_t kMaxTextEmitBytes = 32768;

/// A fixed, caller-owned character sink.
///
/// Emission clears this sink before starting and clears it again on error, so callers never use a
/// prefix of a failed shader.
struct TextSink {
  char* data = nullptr;                            //!< Writable character storage.
  uint32_t capacity = 0;                           //!< Capacity of the data buffer in bytes.
  uint32_t size = 0;                               //!< Number of emitted bytes.
  TextEmitError diagnostic = TextEmitError::None;  //!< First failure reported by emission.

  /// Clears the written range.
  constexpr void clear() {
    size = 0;
    diagnostic = TextEmitError::None;
  }

  /// Appends text or reports that its complete contents do not fit.
  constexpr bool append(std::string_view text) {
    if (text.size() > kMaxTextEmitBytes - size ||
        (data != nullptr && text.size() > capacity - size)) {
      return false;
    }
    for (char character : text) {
      if (data != nullptr) {
        data[size] = character;
      }
      ++size;
    }
    return true;
  }

  /// Appends one character.
  constexpr bool append(char character) {
    if (size == kMaxTextEmitBytes || (data != nullptr && size == capacity)) {
      return false;
    }
    if (data != nullptr) {
      data[size] = character;
    }
    ++size;
    return true;
  }

  /// Returns the complete emitted text.
  constexpr std::string_view view() const {
    return data == nullptr ? std::string_view() : std::string_view(data, size);
  }
};

namespace detail {

class MslTextEmitter {
public:
  constexpr MslTextEmitter(const Module& module, TextSink& sink) : module_(module), sink_(sink) {}

  constexpr TextEmitResult emit() {
    sink_.clear();
    if (!module_.isValid() || module_.sourceByteCount > ModuleLimits::kMaxSourceBytes) {
      return fail(TextEmitError::InvalidModule);
    }
    text("#include <metal_stdlib>\nusing namespace metal;\n\n");
    for (uint16_t index = 0; index < module_.structCount; ++index) {
      if (error_ != TextEmitError::None) return finish();
      emitStruct(index);
      newline();
    }
    emitTextureHelpers();
    for (uint16_t index = 0; index < module_.functionCount; ++index) {
      if (error_ != TextEmitError::None) return finish();
      emitFunction(index);
      newline();
    }
    return finish();
  }

private:
  const Module& module_;
  TextSink& sink_;
  TextEmitError error_ = TextEmitError::None;
  uint8_t indent_ = 0;

  constexpr TextEmitResult fail(TextEmitError error) {
    if (error_ == TextEmitError::None) {
      error_ = error;
    }
    sink_.size = 0;
    sink_.diagnostic = error_;
    return TextEmitResult{error_};
  }

  constexpr TextEmitResult finish() {
    if (error_ != TextEmitError::None) {
      sink_.size = 0;
      sink_.diagnostic = error_;
    }
    return TextEmitResult{error_};
  }

  constexpr void text(std::string_view value) {
    if (error_ == TextEmitError::None && !sink_.append(value)) {
      error_ = TextEmitError::SinkTooSmall;
    }
  }

  constexpr void character(char value) {
    if (error_ == TextEmitError::None && !sink_.append(value)) {
      error_ = TextEmitError::SinkTooSmall;
    }
  }

  constexpr void uintText(uint32_t value) {
    char digits[10] = {};
    uint8_t count = 0;
    do {
      digits[count++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value != 0);
    while (count != 0) {
      character(digits[--count]);
    }
  }

  constexpr void intText(int32_t value) {
    if (value < 0) {
      character('-');
      uintText(static_cast<uint32_t>(-(static_cast<int64_t>(value))));
    } else {
      uintText(static_cast<uint32_t>(value));
    }
  }

  constexpr void indentation() {
    for (uint8_t index = 0; index < indent_; ++index) {
      text("  ");
    }
  }

  constexpr void newline() { character('\n'); }

  constexpr bool validId(ArenaId id, uint16_t count) const {
    return id != kInvalidArenaId && id < count;
  }

  constexpr bool validName(std::string_view name) const {
    if (name.empty()) {
      return false;
    }
    const auto start = [](char ch) constexpr {
      return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
    };
    if (!start(name.front())) {
      return false;
    }
    for (char ch : name) {
      if (!start(ch) && (ch < '0' || ch > '9')) {
        return false;
      }
    }
    return true;
  }

  constexpr bool reservedEntryName(std::string_view name) const {
    constexpr std::string_view kNames[] = {"kernel", "vertex", "fragment", "main",
                                           "void",   "float",  "int",      "uint",
                                           "bool",   "metal",  "namespace"};
    for (const std::string_view reserved : kNames) {
      if (name == reserved) {
        return true;
      }
    }
    return false;
  }

  constexpr void prefixed(std::string_view prefix, NameRef name) {
    const std::string_view sourceName = module_.name(name);
    if (!validName(sourceName)) {
      error_ = TextEmitError::InvalidModule;
      return;
    }
    text(prefix);
    text(sourceName);
  }

  constexpr void type(const Type& value) {
    if (value.lanes < 1 || value.lanes > 4) {
      error_ = TextEmitError::UnsupportedType;
      return;
    }
    std::string_view scalar;
    switch (value.kind) {
      case TypeKind::Void: text("void"); return;
      case TypeKind::Bool: scalar = "bool"; break;
      case TypeKind::I32: scalar = "int"; break;
      case TypeKind::U32: scalar = "uint"; break;
      case TypeKind::F32: scalar = "float"; break;
      case TypeKind::Struct:
        if (!validId(value.structId, module_.structCount)) {
          error_ = TextEmitError::InvalidArenaReference;
          return;
        }
        prefixed("donner_msl_struct_", module_.structs[value.structId].name);
        return;
      case TypeKind::SampledTexture2d: text("texture2d<float, access::read>"); return;
      case TypeKind::StorageTexture2d: text("texture2d<float, access::write>"); return;
    }
    text(scalar);
    if (value.lanes != 1) {
      uintText(value.lanes);
    }
  }

  constexpr uint32_t typeAlignment(const Type& value) const {
    if (!value.isNumeric()) {
      return 0;
    }
    return value.lanes == 1 ? 4 : value.lanes == 2 ? 8 : 16;
  }

  constexpr uint32_t typeSize(const Type& value) const {
    if (!value.isNumeric()) {
      return 0;
    }
    return value.lanes == 1 ? 4 : value.lanes == 2 ? 8 : 16;
  }

  constexpr void emitStruct(uint16_t structId) {
    if (structId >= module_.structCount) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    const Struct& structure = module_.structs[structId];
    if (!validId(structure.firstMember, module_.structMemberCount) ||
        structure.memberCount > module_.structMemberCount - structure.firstMember) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    text("struct ");
    prefixed("donner_msl_struct_", structure.name);
    text(" {\n");
    ++indent_;
    uint32_t offset = 0;
    uint32_t maximumAlignment = 1;
    for (uint16_t index = 0; index < structure.memberCount; ++index) {
      const StructMember& member = module_.structMembers[structure.firstMember + index];
      const uint32_t alignment = typeAlignment(member.type);
      const uint32_t size = typeSize(member.type);
      if (alignment == 0 || size == 0 || member.alignment != alignment || member.size != size) {
        error_ = TextEmitError::UniformLayoutMismatch;
        return;
      }
      offset = ((offset + alignment - 1) / alignment) * alignment;
      if (member.offset != offset) {
        error_ = TextEmitError::UniformLayoutMismatch;
        return;
      }
      offset += size;
      if (alignment > maximumAlignment) {
        maximumAlignment = alignment;
      }
      indentation();
      type(member.type);
      character(' ');
      prefixed("donner_msl_member_", member.name);
      text(";\n");
    }
    const uint32_t naturalSize =
        ((offset + maximumAlignment - 1) / maximumAlignment) * maximumAlignment;
    if (structure.alignment != maximumAlignment || structure.size != naturalSize) {
      error_ = TextEmitError::UniformLayoutMismatch;
      return;
    }
    --indent_;
    text("};\n");
  }

  constexpr void bindingName(uint16_t id) {
    if (id >= module_.bindingCount) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    prefixed("donner_msl_binding_", module_.bindings[id].name);
  }

  constexpr void resourceParameters(bool attributes) {
    for (uint16_t index = 0; index < module_.bindingCount; ++index) {
      if (index != 0) {
        text(", ");
      }
      const Binding& binding = module_.bindings[index];
      if (binding.group != 0 ||
          ((binding.kind == BindingKind::Uniform && binding.binding >= kMslBufferBindingCount) ||
           ((binding.kind == BindingKind::SampledTexture ||
             binding.kind == BindingKind::StorageTexture) &&
            binding.binding >= kMslTextureBindingCount))) {
        error_ = TextEmitError::UnsupportedBinding;
        return;
      }
      switch (binding.kind) {
        case BindingKind::Uniform:
          text("constant ");
          type(binding.type);
          text("& ");
          bindingName(index);
          if (attributes) {
            text(" [[buffer(");
            uintText(MslBufferIndex(binding.binding));
            text(")]]");
          }
          break;
        case BindingKind::SampledTexture:
        case BindingKind::StorageTexture:
          type(binding.type);
          character(' ');
          bindingName(index);
          if (attributes) {
            text(" [[texture(");
            uintText(MslTextureIndex(binding.binding));
            text(")]]");
          }
          break;
      }
    }
  }

  constexpr void forwardingParameters() {
    for (uint16_t index = 0; index < module_.bindingCount; ++index) {
      if (index != 0) {
        text(", ");
      }
      bindingName(index);
    }
  }

  constexpr void emitTextureHelpers() {
    text(
        "float4 donner_msl_texture_load(texture2d<float, access::read> texture, int2 coord, int "
        "level) {\n");
    text("  if (level < 0 || uint(level) >= texture.get_num_mip_levels()) return float4(0.0f);\n");
    text("  uint mip = uint(level);\n");
    text("  uint2 size(texture.get_width(mip), texture.get_height(mip));\n");
    text("  if (any(coord < int2(0)) || any(uint2(coord) >= size)) return float4(0.0f);\n");
    text("  return texture.read(uint2(coord), mip);\n}\n\n");
    text(
        "void donner_msl_texture_store(texture2d<float, access::write> texture, int2 coord, float4 "
        "value) {\n");
    text("  uint2 size(texture.get_width(), texture.get_height());\n");
    text("  if (any(coord < int2(0)) || any(uint2(coord) >= size)) return;\n");
    text("  texture.write(value, uint2(coord));\n}\n\n");
  }

  constexpr void symbolName(ArenaId id) {
    if (!validId(id, module_.symbolCount)) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    const Symbol& symbol = module_.symbols[id];
    if (symbol.kind == SymbolKind::Binding) {
      if (!validId(symbol.bindingId, module_.bindingCount)) {
        error_ = TextEmitError::InvalidArenaReference;
        return;
      }
      bindingName(symbol.bindingId);
      return;
    }
    prefixed("donner_msl_symbol_", symbol.name);
    character('_');
    uintText(id);
  }

  constexpr void floatText(uint32_t bits) {
    if ((bits & 0x7fffffffu) == 0) {
      if ((bits >> 31) != 0) {
        character('-');
      }
      text("0.0f");
      return;
    }
    const uint32_t exponent = (bits >> 23) & 0xffu;
    if (exponent == 0xffu) {
      error_ = TextEmitError::InvalidModule;
      return;
    }
    if ((bits >> 31) != 0) {
      character('-');
    }
    uint32_t mantissa = bits & 0x7fffffu;
    int32_t power = 0;
    if (exponent == 0) {
      uint32_t leading = 0x400000u;
      while ((mantissa & leading) == 0) {
        leading >>= 1;
        ++power;
      }
      mantissa = (mantissa << (power + 1)) & 0x7fffffu;
      power = -127 - power;
    } else {
      power = static_cast<int32_t>(exponent) - 127;
    }
    mantissa <<= 1;
    text("0x1.");
    constexpr char kHex[] = "0123456789abcdef";
    for (int shift = 20; shift >= 0; shift -= 4) {
      character(kHex[(mantissa >> shift) & 0xfu]);
    }
    character('p');
    intText(power);
    character('f');
  }

  constexpr void expression(ArenaId id) {
    if (error_ != TextEmitError::None) return;
    if (!validId(id, module_.expressionCount)) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    const Expression& node = module_.expressions[id];
    const auto child = [&](uint8_t index) constexpr {
      if (index >= node.operandCount) {
        error_ = TextEmitError::InvalidArenaReference;
      } else {
        expression(node.operands[index]);
      }
    };
    switch (node.kind) {
      case ExpressionKind::Literal:
        switch (node.type.kind) {
          case TypeKind::Bool: text(node.payload == 0 ? "false" : "true"); return;
          case TypeKind::I32:
            text("int(");
            intText(std::bit_cast<int32_t>(node.payload));
            character(')');
            return;
          case TypeKind::U32:
            uintText(node.payload);
            text("u");
            return;
          case TypeKind::F32: floatText(node.payload); return;
          default: error_ = TextEmitError::UnsupportedType; return;
        }
      case ExpressionKind::Symbol: symbolName(static_cast<ArenaId>(node.payload)); return;
      case ExpressionKind::Unary:
        if (static_cast<UnaryOp>(node.payload) == UnaryOp::Negate &&
            node.type.kind == TypeKind::I32) {
          text("as_type<");
          type(node.type);
          text(">(");
          Type unsignedType = node.type;
          unsignedType.kind = TypeKind::U32;
          vectorConstant(unsignedType, "0u");
          text(" - as_type<");
          type(unsignedType);
          text(">(");
          child(0);
          text("))");
          return;
        }
        character('(');
        text(static_cast<UnaryOp>(node.payload) == UnaryOp::Negate ? "-" : "!");
        child(0);
        character(')');
        return;
      case ExpressionKind::Binary: {
        constexpr std::string_view kOperators[] = {
            "+", "-", "*", "/", "%", "<", "<=", ">", ">=", "==", "!=", "&&", "||"};
        const uint32_t op = node.payload;
        if (op >= sizeof(kOperators) / sizeof(kOperators[0])) {
          error_ = TextEmitError::InvalidModule;
          return;
        }
        if ((node.type.kind == TypeKind::I32 || node.type.kind == TypeKind::U32) &&
            (op == static_cast<uint32_t>(BinaryOp::Add) ||
             op == static_cast<uint32_t>(BinaryOp::Sub) ||
             op == static_cast<uint32_t>(BinaryOp::Mul) ||
             op == static_cast<uint32_t>(BinaryOp::Div) ||
             op == static_cast<uint32_t>(BinaryOp::Mod))) {
          emitIntegerBinary(node, static_cast<BinaryOp>(op));
          return;
        }
        character('(');
        child(0);
        character(' ');
        text(kOperators[op]);
        character(' ');
        child(1);
        character(')');
        return;
      }
      case ExpressionKind::Member:
        child(0);
        character('.');
        if (!validId(static_cast<ArenaId>(node.payload), module_.structMemberCount)) {
          error_ = TextEmitError::InvalidArenaReference;
          return;
        }
        prefixed("donner_msl_member_", module_.structMembers[node.payload].name);
        return;
      case ExpressionKind::Swizzle: {
        child(0);
        character('.');
        const uint32_t packed = node.payload;
        const uint8_t count = static_cast<uint8_t>(packed >> 8);
        if (count == 0 || count > 4) {
          error_ = TextEmitError::InvalidModule;
          return;
        }
        constexpr char kComponents[] = {'x', 'y', 'z', 'w'};
        for (uint8_t index = 0; index < count; ++index) {
          const uint8_t component = static_cast<uint8_t>((packed >> (index * 2)) & 3u);
          character(kComponents[component]);
        }
        return;
      }
      case ExpressionKind::Construct:
        if (node.operandCount == 1 &&
            module_.expressions[node.operands[0]].type.lanes == node.type.lanes &&
            module_.expressions[node.operands[0]].type.kind != node.type.kind) {
          emitConversion(node);
          return;
        }
        type(node.type);
        character('(');
        for (uint8_t index = 0; index < node.operandCount; ++index) {
          if (index != 0) text(", ");
          child(index);
        }
        character(')');
        return;
      case ExpressionKind::Convert: emitConversion(node); return;
      case ExpressionKind::BuiltinCall: emitBuiltin(node); return;
      case ExpressionKind::FunctionCall: emitFunctionCall(node); return;
    }
  }

  constexpr void vectorConstant(const Type& valueType, std::string_view scalar) {
    type(valueType);
    character('(');
    text(scalar);
    character(')');
  }

  constexpr void emitIntegerBinary(const Expression& node, BinaryOp op) {
    const bool signedValue = node.type.kind == TypeKind::I32;
    if (op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul) {
      text("as_type<");
      type(node.type);
      text(">(");
      Type unsignedType = node.type;
      unsignedType.kind = TypeKind::U32;
      emitUnsignedOperand(node, 0, unsignedType);
      text(" ");
      text(op == BinaryOp::Add ? "+" : op == BinaryOp::Sub ? "-" : "*");
      text(" ");
      emitUnsignedOperand(node, 1, unsignedType);
      text(")");
      return;
    }
    character('(');
    expression(node.operands[0]);
    text(op == BinaryOp::Div ? " / " : " % ");
    text("select(");
    emitResultOperand(node, 1);
    text(", ");
    vectorConstant(node.type, signedValue ? "1" : "1u");
    text(", ((");
    emitResultOperand(node, 1);
    text(" == ");
    vectorConstant(node.type, signedValue ? "0" : "0u");
    text(")");
    if (signedValue) {
      text(node.type.lanes == 1 ? " || ((" : " | ((");
      expression(node.operands[0]);
      text(" == ");
      vectorConstant(node.type, "int(-2147483647 - 1)");
      text(") & (");
      emitResultOperand(node, 1);
      text(" == ");
      vectorConstant(node.type, "-1");
      text("))");
    }
    text("))");
    character(')');
  }

  constexpr void emitResultOperand(const Expression& node, uint8_t operandIndex) {
    if (operandIndex >= node.operandCount ||
        !validId(node.operands[operandIndex], module_.expressionCount)) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    const Type operandType = module_.expressions[node.operands[operandIndex]].type;
    if (operandType.lanes == 1 && node.type.lanes > 1) {
      type(node.type);
      character('(');
      expression(node.operands[operandIndex]);
      character(')');
    } else {
      expression(node.operands[operandIndex]);
    }
  }

  constexpr void emitUnsignedOperand(const Expression& node, uint8_t operandIndex,
                                     const Type& unsignedType) {
    if (operandIndex >= node.operandCount ||
        !validId(node.operands[operandIndex], module_.expressionCount)) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    const Type operandType = module_.expressions[node.operands[operandIndex]].type;
    text("as_type<");
    type(unsignedType);
    text(">(");
    if (operandType.lanes == 1 && unsignedType.lanes > 1) {
      Type splatType = operandType;
      splatType.lanes = unsignedType.lanes;
      type(splatType);
      character('(');
      expression(node.operands[operandIndex]);
      character(')');
    } else {
      expression(node.operands[operandIndex]);
    }
    character(')');
  }

  constexpr void emitConversion(const Expression& node) {
    if (node.operandCount != 1 || !validId(node.operands[0], module_.expressionCount)) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    const Type& source = module_.expressions[node.operands[0]].type;
    if ((node.type.kind == TypeKind::I32 || node.type.kind == TypeKind::U32) &&
        source.kind == TypeKind::F32) {
      Type floatingTarget = node.type;
      floatingTarget.kind = TypeKind::F32;
      type(node.type);
      text("(select(clamp(");
      expression(node.operands[0]);
      text(", ");
      if (node.type.kind == TypeKind::I32) {
        vectorConstant(floatingTarget, "-0x1.000000p31f");
        text(", ");
        vectorConstant(floatingTarget, "0x1.fffffep30f");
      } else {
        vectorConstant(floatingTarget, "0.0f");
        text(", ");
        vectorConstant(floatingTarget, "0x1.fffffep31f");
      }
      text("), ");
      vectorConstant(floatingTarget, "0.0f");
      text(", isnan(");
      expression(node.operands[0]);
      text(")))");
      return;
    }
    if ((node.type.kind == TypeKind::I32 || node.type.kind == TypeKind::U32) &&
        (source.kind == TypeKind::I32 || source.kind == TypeKind::U32) &&
        source.kind != node.type.kind) {
      text("as_type<");
      type(node.type);
      text(">(");
      expression(node.operands[0]);
      text(")");
      return;
    }
    type(node.type);
    character('(');
    expression(node.operands[0]);
    character(')');
  }

  constexpr void expressionList(const Expression& node, uint8_t start = 0) {
    for (uint8_t index = start; index < node.operandCount; ++index) {
      if (error_ != TextEmitError::None) return;
      if (index != start) text(", ");
      expression(node.operands[index]);
    }
  }

  constexpr void emitBuiltin(const Expression& node) {
    const Builtin builtin = static_cast<Builtin>(node.payload);
    switch (builtin) {
      case Builtin::Any:
        text("any(");
        expressionList(node);
        character(')');
        return;
      case Builtin::Clamp:
        text("clamp(");
        expressionList(node);
        character(')');
        return;
      case Builtin::Select:
        text("select(");
        expression(node.operands[0]);
        text(", ");
        expression(node.operands[1]);
        text(", ");
        expression(node.operands[2]);
        character(')');
        return;
      case Builtin::Min:
        text("min(");
        expressionList(node);
        character(')');
        return;
      case Builtin::Ceil:
        text("ceil(");
        expressionList(node);
        character(')');
        return;
      case Builtin::Exp:
        text("exp(");
        expressionList(node);
        character(')');
        return;
      case Builtin::TextureLoad:
        text("donner_msl_texture_load(");
        expressionList(node);
        character(')');
        return;
      case Builtin::TextureDimensions:
        text("uint2(");
        expression(node.operands[0]);
        text(".get_width(), ");
        expression(node.operands[0]);
        text(".get_height())");
        return;
      case Builtin::TextureStore: error_ = TextEmitError::InvalidModule; return;
    }
  }

  constexpr void emitFunctionCall(const Expression& node) {
    if (!validId(static_cast<ArenaId>(node.payload), module_.functionCount)) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    const Function& function = module_.functions[node.payload];
    if (function.stage != Stage::None) {
      error_ = TextEmitError::InvalidModule;
      return;
    }
    prefixed("donner_msl_function_", function.name);
    character('(');
    forwardingParameters();
    for (uint8_t index = 0; index < node.operandCount; ++index) {
      if (module_.bindingCount != 0 || index != 0) text(", ");
      expression(node.operands[index]);
    }
    character(')');
  }

  constexpr void statement(ArenaId id, bool inlineStatement = false) {
    if (error_ != TextEmitError::None) return;
    if (!validId(id, module_.statementCount)) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    const Statement& node = module_.statements[id];
    if (!inlineStatement) indentation();
    switch (node.kind) {
      case StatementKind::Declaration: {
        if (!validId(node.symbolId, module_.symbolCount)) {
          error_ = TextEmitError::InvalidArenaReference;
          return;
        }
        const Symbol& symbol = module_.symbols[node.symbolId];
        text(symbol.kind == SymbolKind::Var ? "" : "const ");
        type(symbol.type);
        character(' ');
        symbolName(node.symbolId);
        text(" = ");
        expression(node.expression);
        if (!inlineStatement) character(';');
        break;
      }
      case StatementKind::Assign:
        expression(node.expression);
        text(" = ");
        expression(node.secondExpression);
        if (!inlineStatement) character(';');
        break;
      case StatementKind::If:
        text("if (");
        expression(node.expression);
        text(") {\n");
        ++indent_;
        block(node.firstBody);
        --indent_;
        indentation();
        character('}');
        if (node.firstElseBody != kInvalidArenaId) {
          text(" else {\n");
          ++indent_;
          block(node.firstElseBody);
          --indent_;
          indentation();
          character('}');
        }
        break;
      case StatementKind::For:
        text("for (");
        statement(node.init, true);
        text("; ");
        expression(node.expression);
        text("; ");
        statement(node.continuing, true);
        text(") {\n");
        ++indent_;
        block(node.firstBody);
        --indent_;
        indentation();
        character('}');
        break;
      case StatementKind::Return:
        text("return");
        if (node.expression != kInvalidArenaId) {
          character(' ');
          expression(node.expression);
        }
        if (!inlineStatement) character(';');
        break;
      case StatementKind::TextureStore:
        text("donner_msl_texture_store(");
        expression(node.expression);
        text(", ");
        expression(node.secondExpression);
        text(", ");
        expression(node.thirdExpression);
        character(')');
        if (!inlineStatement) character(';');
        break;
    }
    if (!inlineStatement) newline();
  }

  constexpr void block(ArenaId first) {
    ArenaId current = first;
    uint16_t count = 0;
    while (current != kInvalidArenaId) {
      if (error_ != TextEmitError::None) return;
      if (count++ >= module_.statementCount || current >= module_.statementCount) {
        error_ = TextEmitError::InvalidArenaReference;
        return;
      }
      statement(current);
      current = module_.statements[current].next;
    }
  }

  constexpr void emitFunction(uint16_t functionId) {
    if (error_ != TextEmitError::None) return;
    if (functionId >= module_.functionCount) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    const Function& function = module_.functions[functionId];
    const bool entry = function.stage == Stage::Compute;
    if (function.stage != Stage::None && !entry) {
      error_ = TextEmitError::InvalidModule;
      return;
    }
    if (entry) {
      const std::string_view name = module_.name(function.name);
      if (!validName(name) || reservedEntryName(name) || name.starts_with("donner_msl_")) {
        error_ = TextEmitError::UnsupportedEntryPointName;
        return;
      }
      text("kernel void ");
      text(name);
      text("(");
    } else {
      type(function.returnType);
      character(' ');
      prefixed("donner_msl_function_", function.name);
      text("(");
    }
    resourceParameters(entry);
    if (!validId(function.firstParameter, module_.symbolCount) && function.parameterCount != 0) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    for (uint16_t index = 0; index < function.parameterCount; ++index) {
      const ArenaId symbolId = function.firstParameter + index;
      if (symbolId >= module_.symbolCount) {
        error_ = TextEmitError::InvalidArenaReference;
        return;
      }
      if (module_.bindingCount != 0 || index != 0) text(", ");
      const Symbol& symbol = module_.symbols[symbolId];
      if ((entry && symbol.builtin != BuiltinInput::GlobalInvocationId) ||
          (!entry && symbol.builtin != BuiltinInput::None)) {
        error_ = TextEmitError::InvalidModule;
        return;
      }
      type(symbol.type);
      character(' ');
      symbolName(symbolId);
      if (entry) text(" [[thread_position_in_grid]]");
    }
    text(") {\n");
    ++indent_;
    block(function.firstStatement);
    --indent_;
    text("}\n");
  }
};

}  // namespace detail

/// Emits a deterministic, bounded Metal Shading Language projection of p module.
///
/// @param module Validated parsed WGSL module.
/// @param sink Caller-owned output storage.
constexpr TextEmitResult EmitMsl(const Module& module, TextSink& sink) {
  return detail::MslTextEmitter(module, sink).emit();
}

/// Copies the validated WGSL projection retained by p module into p sink.
///
/// @param module Validated parsed WGSL module.
/// @param sink Caller-owned output storage.
constexpr TextEmitResult EmitWgsl(const Module& module, TextSink& sink) {
  sink.clear();
  const TextEmitError error =
      !module.isValid() || module.sourceByteCount > ModuleLimits::kMaxSourceBytes
          ? TextEmitError::InvalidModule
      : !sink.append(module.source()) ? TextEmitError::SinkTooSmall
                                      : TextEmitError::None;
  if (error != TextEmitError::None) {
    sink.size = 0;
    sink.diagnostic = error;
    return TextEmitResult{error};
  }
  return TextEmitResult{};
}

}  // namespace donner::gpu::shader::wgsl
