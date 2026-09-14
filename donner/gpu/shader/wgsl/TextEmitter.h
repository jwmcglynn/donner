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
inline constexpr uint32_t kMaxTextEmitBytes = 65536;

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
    emitArrayHelpers();
    emitTextureHelpers();
    for (uint16_t index = 0; index < module_.functionCount; ++index) {
      if (error_ != TextEmitError::None) return finish();
      emitFunction(index);
      newline();
    }
    for (uint16_t index = 0; index < module_.functionCount; ++index) {
      const Stage stage = module_.functions[index].stage;
      if (stage == Stage::Vertex || stage == Stage::Fragment) emitGraphicsWrapper(index);
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
    emitTypeKind(value);
  }

  constexpr void emitTypeKind(const Type& value) {
    switch (value.kind) {
      case TypeKind::AbstractInt:
      case TypeKind::AbstractFloat: error_ = TextEmitError::UnsupportedType; return;
      case TypeKind::Void: text("void"); return;
      case TypeKind::Struct: emitStructType(value); return;
      case TypeKind::Matrix: emitMatrixType(value); return;
      case TypeKind::Array: error_ = TextEmitError::UnsupportedType; return;
      case TypeKind::Sampler: text("sampler"); return;
      case TypeKind::SampledTexture2d: text("texture2d<float, access::read>"); return;
      case TypeKind::StorageTexture2d:
        if (!value.hasSupportedStorageFormat()) error_ = TextEmitError::UnsupportedType;
        text("texture2d<float, access::write>");
        return;
      default: emitScalarType(value); return;
    }
  }

  constexpr void emitScalarType(const Type& value) {
    constexpr std::string_view kNames[] = {"bool", "int", "uint", "float"};
    const uint8_t index = static_cast<uint8_t>(value.kind) - static_cast<uint8_t>(TypeKind::Bool);
    if (index >= sizeof(kNames) / sizeof(kNames[0])) {
      error_ = TextEmitError::UnsupportedType;
      return;
    }
    text(kNames[index]);
    if (value.lanes != 1) {
      uintText(value.lanes);
    }
  }

  constexpr void emitStructType(const Type& value) {
    if (!validId(value.structId, module_.structCount)) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    prefixed("donner_msl_struct_", module_.structs[value.structId].name);
  }

  constexpr void emitMatrixType(const Type& value) {
    text("float");
    uintText(value.columns);
    character('x');
    uintText(value.rows);
  }

  constexpr uint32_t typeAlignment(const Type& value) const {
    if (value.kind == TypeKind::Matrix) return value.rows == 2 ? 8 : 16;
    if (value.kind == TypeKind::Array) return typeAlignment(value.elementType());
    if (!value.isNumeric()) {
      return 0;
    }
    return value.lanes == 1 ? 4 : value.lanes == 2 ? 8 : 16;
  }

  constexpr uint32_t typeSize(const Type& value) const {
    if (value.kind == TypeKind::Matrix) return typeAlignment(value) * value.columns;
    if (value.kind == TypeKind::Array) return module_.arrayStride(value) * value.arrayCount;
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
    const bool buffer = structIsBuffer(structId);
    if (!validId(structure.firstMember, module_.structMemberCount) ||
        structure.memberCount > module_.structMemberCount - structure.firstMember) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    text("struct ");
    prefixed("donner_msl_struct_", structure.name);
    text(" {\n");
    ++indent_;
    StructLayoutState layout;
    emitStructMembers(structure, buffer, &layout);
    if (error_ != TextEmitError::None) return;
    validateStructLayout(structure, buffer, layout);
    if (error_ != TextEmitError::None) return;
    --indent_;
    text("};\n");
  }

  struct StructLayoutState {
    uint32_t offset = 0;
    uint32_t maximumAlignment = 1;
  };

  constexpr void emitStructMembers(const Struct& structure, bool buffer,
                                   StructLayoutState* layout) {
    for (uint16_t index = 0; index < structure.memberCount; ++index) {
      emitStructMemberWithLayout(module_.structMembers[structure.firstMember + index], buffer,
                                 layout);
      if (error_ != TextEmitError::None) return;
    }
  }

  constexpr void emitStructMemberWithLayout(const StructMember& member, bool buffer,
                                            StructLayoutState* layout) {
    const uint32_t alignment = typeAlignment(member.type);
    const uint32_t size = typeSize(member.type);
    if (!validMemberLayout(member, buffer, alignment, size)) return;
    layout->offset = ((layout->offset + alignment - 1) / alignment) * alignment;
    if (buffer && member.offset != layout->offset) {
      error_ = TextEmitError::UniformLayoutMismatch;
      return;
    }
    layout->offset += size;
    if (alignment > layout->maximumAlignment) layout->maximumAlignment = alignment;
    emitStructMember(member);
  }

  constexpr bool validMemberLayout(const StructMember& member, bool buffer, uint32_t alignment,
                                   uint32_t size) {
    if (alignment == 0 || size == 0 ||
        (buffer && (member.alignment != alignment || member.size != size))) {
      error_ = TextEmitError::UniformLayoutMismatch;
      return false;
    }
    return true;
  }

  constexpr void validateStructLayout(const Struct& structure, bool buffer,
                                      const StructLayoutState& layout) {
    const uint32_t naturalSize =
        ((layout.offset + layout.maximumAlignment - 1) / layout.maximumAlignment) *
        layout.maximumAlignment;
    if (buffer && (structure.alignment != layout.maximumAlignment || structure.size != naturalSize))
      error_ = TextEmitError::UniformLayoutMismatch;
  }

  constexpr bool structIsBuffer(uint16_t structId) const {
    for (uint16_t i = 0; i < module_.bindingCount; ++i) {
      const Type& type = module_.bindings[i].type;
      if ((type.kind == TypeKind::Struct ||
           (type.kind == TypeKind::Array && type.elementKind == TypeKind::Struct)) &&
          type.structId == structId)
        return true;
    }
    return false;
  }

  constexpr void emitStructMember(const StructMember& member) {
    indentation();
    if (member.type.kind == TypeKind::Array) {
      if (member.arrayStride != module_.arrayStride(member.type)) {
        error_ = TextEmitError::UniformLayoutMismatch;
        return;
      }
      type(member.type.elementType());
      character(' ');
      prefixed("donner_msl_member_", member.name);
      character('[');
      uintText(member.type.arrayCount);
      character(']');
    } else {
      type(member.type);
      character(' ');
      prefixed("donner_msl_member_", member.name);
    }
    text(";\n");
  }

  constexpr bool usesRuntimeArrays(uint32_t mask = ~uint32_t(0)) const {
    for (uint16_t i = 0; i < module_.bindingCount; ++i)
      if ((mask & (uint32_t(1) << i)) && module_.bindings[i].type.kind == TypeKind::Array &&
          module_.bindings[i].type.arrayCount == 0)
        return true;
    return false;
  }

  constexpr void emitArrayHelpers() {
    if (!usesRuntimeArrays()) return;
    text(
        "template<typename T> T donner_msl_array_load(const device T* data, uint count, uint "
        "index) {\n");
    text("  if (count == 0u) return T{};\n  return data[min(index, count - 1u)];\n}\n");
    text(
        "template<typename T> T donner_msl_array_load(const device T* data, uint count, int index) "
        "{\n");
    text("  return donner_msl_array_load(data, count, uint(max(index, 0)));\n}\n\n");
  }

  constexpr void bindingName(uint16_t id) {
    if (id >= module_.bindingCount) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    prefixed("donner_msl_binding_", module_.bindings[id].name);
  }

  constexpr void resourceParameters(bool attributes, uint32_t mask) {
    bool comma = false;
    for (uint16_t index = 0; index < module_.bindingCount; ++index) {
      if (!(mask & (uint32_t(1) << index))) continue;
      if (comma) {
        text(", ");
      }
      comma = true;
      const Binding& binding = module_.bindings[index];
      const bool isBuffer =
          binding.kind == BindingKind::Uniform || binding.kind == BindingKind::ReadOnlyStorage;
      const uint32_t bindingLimit = isBuffer ? kMslBufferBindingCount
                                    : binding.kind == BindingKind::Sampler
                                        ? kMslSamplerBindingCount
                                        : kMslTextureBindingCount;
      if (binding.group != 0 || binding.binding >= bindingLimit) {
        error_ = TextEmitError::UnsupportedBinding;
        return;
      }
      emitResourceParameter(index, attributes);
    }
    if (usesRuntimeArrays(mask)) {
      if (comma) text(", ");
      text("constant uint* donner_msl_lengths");
      if (attributes) {
        text(" [[buffer(");
        uintText(kMslBufferLengthsIndex);
        text(")]]");
      }
    }
  }

  constexpr void emitResourceParameter(uint16_t index, bool attributes) {
    const Binding& binding = module_.bindings[index];
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
      case BindingKind::ReadOnlyStorage:
        text("const device ");
        type(binding.type.kind == TypeKind::Array ? binding.type.elementType() : binding.type);
        text(binding.type.kind == TypeKind::Array ? "* " : "& ");
        bindingName(index);
        if (attributes) {
          text(" [[buffer(");
          uintText(MslBufferIndex(binding.binding));
          text(")]]");
        }
        break;
      case BindingKind::Sampler:
        type(binding.type);
        character(' ');
        bindingName(index);
        if (attributes) {
          text(" [[sampler(");
          uintText(MslSamplerIndex(binding.binding));
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

  constexpr void forwardingParameters(uint32_t mask) {
    bool comma = false;
    for (uint16_t index = 0; index < module_.bindingCount; ++index) {
      if (!(mask & (uint32_t(1) << index))) continue;
      if (comma) text(", ");
      bindingName(index);
      comma = true;
    }
    if (usesRuntimeArrays(mask)) {
      if (comma) text(", ");
      text("donner_msl_lengths");
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
    emitExpressionNode(module_.expressions[id]);
  }

  constexpr void emitExpressionNode(const Expression& node) {
    if (node.kind <= ExpressionKind::Construct) {
      emitValueExpression(node);
      return;
    }
    emitCallExpression(node);
  }

  constexpr void emitValueExpression(const Expression& node) {
    switch (node.kind) {
      case ExpressionKind::Zero:
        type(node.type);
        text("{}");
        return;
      case ExpressionKind::Literal: emitLiteral(node); return;
      case ExpressionKind::Symbol: symbolName(static_cast<ArenaId>(node.payload)); return;
      case ExpressionKind::Unary: emitUnary(node); return;
      case ExpressionKind::Binary: emitBinary(node); return;
      case ExpressionKind::Member: emitMember(node); return;
      case ExpressionKind::Swizzle: emitSwizzle(node); return;
      case ExpressionKind::Construct: emitConstruct(node); return;
      default: error_ = TextEmitError::InvalidModule; return;
    }
  }

  constexpr void emitCallExpression(const Expression& node) {
    switch (node.kind) {
      case ExpressionKind::Convert: emitConversion(node); return;
      case ExpressionKind::BuiltinCall: emitBuiltin(node); return;
      case ExpressionKind::FunctionCall: emitFunctionCall(node); return;
      case ExpressionKind::Index: emitIndex(node); return;
      default: error_ = TextEmitError::InvalidModule; return;
    }
  }

  constexpr void emitChild(const Expression& node, uint8_t index) {
    if (index >= node.operandCount) {
      error_ = TextEmitError::InvalidArenaReference;
    } else {
      expression(node.operands[index]);
    }
  }

  constexpr void emitUnary(const Expression& node) {
    if (static_cast<UnaryOp>(node.payload) == UnaryOp::Negate && node.type.kind == TypeKind::I32) {
      text("as_type<");
      type(node.type);
      text(">(");
      Type unsignedType = node.type;
      unsignedType.kind = TypeKind::U32;
      vectorConstant(unsignedType, "0u");
      text(" - as_type<");
      type(unsignedType);
      text(">(");
      emitChild(node, 0);
      text("))");
      return;
    }
    character('(');
    text(static_cast<UnaryOp>(node.payload) == UnaryOp::Negate ? "-" : "!");
    emitChild(node, 0);
    character(')');
    return;
  }

  constexpr void emitMember(const Expression& node) {
    emitChild(node, 0);
    character('.');
    if (!validId(static_cast<ArenaId>(node.payload), module_.structMemberCount)) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    prefixed("donner_msl_member_", module_.structMembers[node.payload].name);
    return;
  }

  constexpr void emitSwizzle(const Expression& node) {
    emitChild(node, 0);
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

  constexpr void emitConstruct(const Expression& node) {
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
      emitChild(node, index);
    }
    character(')');
    return;
  }

  constexpr void emitLiteral(const Expression& node) {
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
  }

  constexpr void emitBinary(const Expression& node) {
    constexpr std::string_view kOperators[] = {
        "+", "-", "*", "/", "%", "<", "<=", ">", ">=", "==", "!=", "&&", "||", "&"};
    const uint32_t op = node.payload;
    if (op >= sizeof(kOperators) / sizeof(kOperators[0])) {
      error_ = TextEmitError::InvalidModule;
      return;
    }
    if ((node.type.kind == TypeKind::I32 || node.type.kind == TypeKind::U32) &&
        (op == static_cast<uint32_t>(BinaryOp::Add) || op == static_cast<uint32_t>(BinaryOp::Sub) ||
         op == static_cast<uint32_t>(BinaryOp::Mul) || op == static_cast<uint32_t>(BinaryOp::Div) ||
         op == static_cast<uint32_t>(BinaryOp::Mod))) {
      emitIntegerBinary(node, static_cast<BinaryOp>(op));
      return;
    }
    character('(');
    expression(node.operands[0]);
    character(' ');
    text(kOperators[op]);
    character(' ');
    expression(node.operands[1]);
    character(')');
  }

  constexpr void emitRuntimeArrayIndex(const Expression& node, const Type& array) {
    const Expression& base = module_.expressions[node.operands[0]];
    if (base.kind != ExpressionKind::Symbol || base.payload >= module_.symbolCount) {
      error_ = TextEmitError::InvalidModule;
      return;
    }
    const Symbol& symbol = module_.symbols[base.payload];
    if (symbol.kind != SymbolKind::Binding || symbol.bindingId >= module_.bindingCount ||
        module_.arrayStride(array) == 0) {
      error_ = TextEmitError::InvalidModule;
      return;
    }
    text("donner_msl_array_load(");
    bindingName(symbol.bindingId);
    text(", donner_msl_lengths[");
    uintText(module_.bindings[symbol.bindingId].binding);
    text("] / ");
    uintText(module_.arrayStride(array));
    text("u, ");
    expression(node.operands[1]);
    character(')');
  }

  constexpr void emitIndex(const Expression& node) {
    if (!validIndexOperands(node)) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    const Type& array = module_.expressions[node.operands[0]].type;
    const Type& index = module_.expressions[node.operands[1]].type;
    if (array.kind == TypeKind::Matrix) {
      emitMatrixIndex(node, array);
      return;
    }
    if (array.kind == TypeKind::Array && array.arrayCount == 0) {
      emitRuntimeArrayIndex(node, array);
      return;
    }
    if (!validFixedArrayIndex(array, index)) {
      error_ = TextEmitError::UnsupportedType;
      return;
    }
    emitFixedArrayIndex(node, array, index);
  }

  constexpr bool validIndexOperands(const Expression& node) const {
    return node.operandCount == 2 && validId(node.operands[0], module_.expressionCount) &&
           validId(node.operands[1], module_.expressionCount);
  }

  constexpr void emitMatrixIndex(const Expression& node, const Type& matrix) {
    if (node.payload >= matrix.columns) {
      error_ = TextEmitError::InvalidModule;
      return;
    }
    expression(node.operands[0]);
    character('[');
    uintText(node.payload);
    character(']');
  }

  constexpr bool validFixedArrayIndex(const Type& array, const Type& index) const {
    return array.kind == TypeKind::Array && array.elementType().isNumeric() &&
           array.arrayCount != 0 && (index.kind == TypeKind::I32 || index.kind == TypeKind::U32) &&
           index.lanes == 1;
  }

  constexpr void emitFixedArrayIndex(const Expression& node, const Type& array, const Type& index) {
    expression(node.operands[0]);
    text("[");
    if (index.kind == TypeKind::I32)
      emitSignedArrayIndex(node, array.arrayCount);
    else
      emitUnsignedArrayIndex(node, array.arrayCount);
    text("]");
  }

  constexpr void emitSignedArrayIndex(const Expression& node, uint16_t count) {
    text("uint(clamp(");
    expression(node.operands[1]);
    text(", int(0), int(");
    uintText(count - 1);
    text(")))");
  }

  constexpr void emitUnsignedArrayIndex(const Expression& node, uint16_t count) {
    text("min(");
    expression(node.operands[1]);
    text(", ");
    uintText(count - 1);
    text("u)");
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

  /// Returns a Metal function name backed by static string literals.
  static constexpr std::string_view BuiltinName(Builtin builtin) {
    struct NamedBuiltin {
      Builtin builtin;
      std::string_view name;
    };
    constexpr NamedBuiltin kNames[] = {
        {Builtin::Sin, "sin"},
        {Builtin::Cos, "cos"},
        {Builtin::Pow, "pow"},
        {Builtin::Floor, "floor"},
        {Builtin::Sign, "sign"},
        {Builtin::All, "all"},
        {Builtin::Abs, "abs"},
        {Builtin::Max, "max"},
        {Builtin::Round, "rint"},
        {Builtin::Sqrt, "sqrt"},
        {Builtin::Dot, "dot"},
        {Builtin::Length, "length"},
        {Builtin::Normalize, "normalize"},
        {Builtin::Saturate, "saturate"},
        {Builtin::Fract, "fract"},
        {Builtin::Fwidth, "fwidth"},
        {Builtin::Any, "any"},
        {Builtin::Clamp, "clamp"},
        {Builtin::Min, "min"},
        {Builtin::Ceil, "ceil"},
        {Builtin::Exp, "exp"},
        {Builtin::TextureLoad, "donner_msl_texture_load"},
    };
    for (const NamedBuiltin& entry : kNames) {
      if (entry.builtin == builtin) return entry.name;
    }
    return {};
  }

  constexpr void emitBuiltin(const Expression& node) {
    const Builtin builtin = static_cast<Builtin>(node.payload);
    switch (builtin) {
      case Builtin::Select:
        text("select(");
        expression(node.operands[0]);
        text(", ");
        expression(node.operands[1]);
        text(", ");
        expression(node.operands[2]);
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
      default:
        const std::string_view name = BuiltinName(builtin);
        if (!name.empty()) emitNamedBuiltin(name, node);
        return;
    }
  }

  constexpr void emitNamedBuiltin(std::string_view name, const Expression& node) {
    text(name);
    character('(');
    expressionList(node);
    character(')');
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
    forwardingParameters(function.resourceMask);
    for (uint8_t index = 0; index < node.operandCount; ++index) {
      if (function.resourceMask != 0 || index != 0) text(", ");
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
    emitStatementNode(node, inlineStatement);
    if (!inlineStatement) newline();
  }

  constexpr void emitStatementNode(const Statement& node, bool inlineStatement) {
    switch (node.kind) {
      case StatementKind::Declaration: emitDeclaration(node, inlineStatement); break;
      case StatementKind::Assign: emitAssignment(node, inlineStatement); break;
      case StatementKind::If: emitIf(node); break;
      case StatementKind::For: emitFor(node); break;
      case StatementKind::Break: text("break;"); break;
      case StatementKind::Continue: text("continue;"); break;
      case StatementKind::Discard:
        text("discard_fragment(); return");
        if (currentReturnType_.kind != TypeKind::Void) text(" {}");
        character(';');
        break;
      case StatementKind::Return: emitReturn(node, inlineStatement); break;
      case StatementKind::TextureStore: emitTextureStore(node, inlineStatement); break;
    }
  }

  constexpr void emitDeclaration(const Statement& node, bool inlineStatement) {
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
  }

  constexpr void emitAssignment(const Statement& node, bool inlineStatement) {
    expression(node.expression);
    text(" = ");
    expression(node.secondExpression);
    if (!inlineStatement) character(';');
  }

  constexpr void emitIf(const Statement& node) {
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
  }

  constexpr void emitFor(const Statement& node) {
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
  }

  constexpr void emitReturn(const Statement& node, bool inlineStatement) {
    text("return");
    if (node.expression != kInvalidArenaId) {
      character(' ');
      expression(node.expression);
    }
    if (!inlineStatement) character(';');
  }

  constexpr void emitTextureStore(const Statement& node, bool inlineStatement) {
    text("donner_msl_texture_store(");
    expression(node.expression);
    text(", ");
    expression(node.secondExpression);
    text(", ");
    expression(node.thirdExpression);
    character(')');
    if (!inlineStatement) character(';');
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

  constexpr void ioTypeName(uint16_t functionId, bool input) {
    text(input ? "donner_msl_input_" : "donner_msl_output_");
    uintText(functionId);
  }

  constexpr void ioField(uint16_t variableId) {
    text("donner_msl_io_");
    uintText(variableId);
  }

  constexpr void ioAttribute(const InterfaceVariable& variable, Stage stage, bool input) {
    if (variable.decoration.builtin == BuiltinValue::Position) {
      text(" [[position]]");
    } else if (variable.decoration.location != UINT32_MAX) {
      text(stage == Stage::Vertex && input      ? " [[attribute("
           : stage == Stage::Fragment && !input ? " [[color("
                                                : " [[user(locn");
      uintText(variable.decoration.location);
      text(")]]");
    } else {
      error_ = TextEmitError::InvalidModule;
    }
  }

  constexpr bool validIoRange(uint16_t first, uint16_t count) {
    if (first > module_.interfaceVariableCount || count > module_.interfaceVariableCount - first) {
      error_ = TextEmitError::InvalidArenaReference;
      return false;
    }
    return true;
  }

  constexpr uint16_t emitIoStruct(uint16_t functionId, bool input) {
    const Function& function = module_.functions[functionId];
    const uint16_t first = input ? function.firstInput : function.firstOutput;
    const uint16_t count = input ? function.inputCount : function.outputCount;
    if (!validIoRange(first, count)) return 0;
    uint16_t fields = 0;
    for (uint16_t i = first; i < first + count; ++i)
      fields += module_.interfaceVariables[i].decoration.builtin != BuiltinValue::VertexIndex;
    if (fields == 0) return 0;
    text("struct ");
    ioTypeName(functionId, input);
    text(" {\n");
    for (uint16_t i = first; i < first + count; ++i) {
      const InterfaceVariable& variable = module_.interfaceVariables[i];
      if (variable.decoration.builtin == BuiltinValue::VertexIndex) continue;
      text("  ");
      type(variable.type);
      character(' ');
      ioField(i);
      ioAttribute(variable, function.stage, input);
      text(";\n");
    }
    text("};\n");
    return fields;
  }

  constexpr void emitIoInput(uint16_t variableId) {
    if (module_.interfaceVariables[variableId].decoration.builtin == BuiltinValue::VertexIndex) {
      text("donner_msl_vertex_index");
    } else {
      text("donner_msl_inputs.");
      ioField(variableId);
    }
  }

  constexpr void emitGraphicsArguments(const Function& function) {
    forwardingParameters(function.resourceMask);
    for (uint16_t parameter = 0; parameter < function.parameterCount; ++parameter) {
      if (parameter != 0 || function.resourceMask != 0) text(", ");
      const ArenaId symbolId = function.firstParameter + parameter;
      const Symbol& symbol = module_.symbols[symbolId];
      const bool structure = symbol.type.kind == TypeKind::Struct;
      if (structure) {
        type(symbol.type);
        character('{');
      }
      uint16_t count = 0;
      for (uint16_t i = function.firstInput; i < function.firstInput + function.inputCount; ++i) {
        if (module_.interfaceVariables[i].symbol != symbolId) continue;
        if (count++ != 0) text(", ");
        emitIoInput(i);
      }
      if (structure) character('}');
    }
  }

  constexpr void emitGraphicsReturn(const Function& function, uint16_t functionId) {
    if (function.outputCount == 0) return;
    text("  return ");
    ioTypeName(functionId, false);
    character('{');
    for (uint16_t i = 0; i < function.outputCount; ++i) {
      if (i != 0) text(", ");
      text("donner_msl_result");
      const InterfaceVariable& variable = module_.interfaceVariables[function.firstOutput + i];
      if (variable.member != kInvalidArenaId) {
        character('.');
        prefixed("donner_msl_member_", variable.name);
      }
    }
    text("};\n");
  }

  constexpr void emitGraphicsWrapper(uint16_t functionId) {
    const Function& function = module_.functions[functionId];
    const uint16_t inputFields = emitIoStruct(functionId, true);
    const uint16_t outputFields = emitIoStruct(functionId, false);
    if (error_ != TextEmitError::None) return;
    const auto name = module_.name(function.name);
    if (!validName(name) || reservedEntryName(name) || name.starts_with("donner_msl_")) {
      error_ = TextEmitError::UnsupportedEntryPointName;
      return;
    }
    text(function.stage == Stage::Vertex ? "vertex " : "fragment ");
    if (outputFields != 0)
      ioTypeName(functionId, false);
    else
      text("void");
    character(' ');
    text(name);
    character('(');
    resourceParameters(true, function.resourceMask);
    bool comma = function.resourceMask != 0;
    if (inputFields != 0) {
      if (comma) text(", ");
      ioTypeName(functionId, true);
      text(" donner_msl_inputs [[stage_in]]");
      comma = true;
    }
    for (uint16_t i = function.firstInput; i < function.firstInput + function.inputCount; ++i) {
      if (module_.interfaceVariables[i].decoration.builtin != BuiltinValue::VertexIndex) continue;
      if (comma) text(", ");
      text("uint donner_msl_vertex_index [[vertex_id]]");
      comma = true;
    }
    text(") {\n  ");
    if (outputFields != 0) text("const auto donner_msl_result = ");
    prefixed("donner_msl_function_", function.name);
    character('(');
    emitGraphicsArguments(function);
    text(");\n");
    emitGraphicsReturn(function, functionId);
    text("}\n\n");
  }

  Type currentReturnType_;

  constexpr void emitFunction(uint16_t functionId) {
    if (error_ != TextEmitError::None) return;
    if (functionId >= module_.functionCount) {
      error_ = TextEmitError::InvalidArenaReference;
      return;
    }
    const Function& function = module_.functions[functionId];
    const bool entry = function.stage == Stage::Compute;
    if (function.stage != Stage::None && !entry && function.stage != Stage::Vertex &&
        function.stage != Stage::Fragment) {
      error_ = TextEmitError::InvalidModule;
      return;
    }
    currentReturnType_ = function.returnType;
    emitFunctionHead(function, entry);
    resourceParameters(entry, function.resourceMask);
    emitFunctionParameters(function, entry);
    text(") {\n");
    ++indent_;
    block(function.firstStatement);
    --indent_;
    text("}\n");
  }

  constexpr void emitFunctionHead(const Function& function, bool entry) {
    if (entry) {
      const std::string_view name = module_.name(function.name);
      if (!validName(name) || reservedEntryName(name) || name.starts_with("donner_msl_")) {
        error_ = TextEmitError::UnsupportedEntryPointName;
        return;
      }
      text("kernel void ");
      text(name);
      text("(");
      return;
    }
    type(function.returnType);
    character(' ');
    prefixed("donner_msl_function_", function.name);
    text("(");
  }

  constexpr void emitFunctionParameters(const Function& function, bool entry) {
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
      if (function.resourceMask != 0 || index != 0) text(", ");
      const Symbol& symbol = module_.symbols[symbolId];
      if ((entry && symbol.builtin != BuiltinValue::GlobalInvocationId) ||
          (function.stage == Stage::None && symbol.builtin != BuiltinValue::None)) {
        error_ = TextEmitError::InvalidModule;
        return;
      }
      type(symbol.type);
      character(' ');
      symbolName(symbolId);
      if (entry) text(" [[thread_position_in_grid]]");
    }
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
