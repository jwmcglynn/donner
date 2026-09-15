#pragma once
/// @file
/// Bounded constant-evaluable SPIR-V emission for validated compute modules.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "donner/gpu/shader/wgsl/Module.h"

namespace donner::gpu::shader::wgsl {

/// A binary-emission failure. No output is usable after an error.
enum class SpirvEmitError : uint8_t { None, InvalidModule, UnsupportedType, InvalidNode, Capacity };

/// Word capacity of a frozen SPIR-V projection. The compiler's emission buffer and the parser
/// fuzzer's sink share this bound.
inline constexpr uint32_t kMaxSpirvEmitWords = 24576;

/// Caller-owned binary output or a counting sink when data is null.
struct SpirvSink {
  uint32_t* data = nullptr;                     //!< Output words, borrowed for the call.
  uint32_t capacity = 0;                        //!< Output capacity in words.
  uint32_t size = 0;                            //!< Words written.
  SpirvEmitError error = SpirvEmitError::None;  //!< Latched failure.

  /// Appends one word, failing closed on capacity exhaustion.
  /// @param value Word to append.
  constexpr void word(uint32_t value) {
    if (error != SpirvEmitError::None) return;
    if (size == UINT32_MAX || (data && size >= capacity)) {
      error = SpirvEmitError::Capacity;
      return;
    }
    if (data) data[size] = value;
    ++size;
  }
};

/// Emission status, retaining the source location of an unsupported operation.
struct SpirvEmitResult {
  SpirvEmitError error = SpirvEmitError::None;
  SourceSpan span;
  constexpr bool isSuccess() const { return error == SpirvEmitError::None; }
};

namespace spirv_detail {

template <size_t Capacity>
struct Words {
  std::array<uint32_t, Capacity> data{};
  uint32_t size = 0;
  bool valid = true;

  constexpr void word(uint32_t value) {
    if (size >= Capacity) {
      valid = false;
      return;
    }
    data[size++] = value;
  }

  template <typename... Operands>
  constexpr void instruction(uint32_t opcode, Operands... operands) {
    word((uint32_t(sizeof...(operands) + 1) << 16) | opcode);
    (word(static_cast<uint32_t>(operands)), ...);
  }

  constexpr void string(std::string_view value) {
    for (size_t offset = 0; offset <= value.size(); offset += 4) {
      uint32_t packed = 0;
      for (size_t byte = 0; byte < 4 && offset + byte < value.size(); ++byte)
        packed |= uint32_t(static_cast<unsigned char>(value[offset + byte])) << (byte * 8);
      word(packed);
    }
  }
};

struct TypeRecord {
  Type type;
  uint32_t id = 0;
};
struct PointerRecord {
  uint32_t type = 0;
  uint32_t storage = 0;
  uint32_t id = 0;
};
struct ConstantRecord {
  Type type;
  uint32_t bits = 0;
  uint32_t id = 0;
};
struct ArrayTemporary {
  ArenaId expression = kInvalidArenaId;
  uint32_t pointer = 0;
};

struct FunctionTypeRecord {
  uint32_t result = 0;
  std::array<uint32_t, Expression::kMaxOperands> parameters{};
  uint16_t count = 0;
  uint32_t id = 0;
};

/// Emits declarations separately from executable instructions so forward type requests are safe.
class Emitter {
private:
  template <typename... Operands>
  static constexpr void writeInstruction(SpirvSink& sink, uint32_t opcode, Operands... operands) {
    sink.word((uint32_t(sizeof...(operands) + 1) << 16) | opcode);
    (sink.word(static_cast<uint32_t>(operands)), ...);
  }
  template <size_t N>
  static constexpr void append(SpirvSink& sink, const Words<N>& block) {
    for (uint32_t i = 0; i < block.size; ++i) sink.word(block.data[i]);
  }

public:
  constexpr explicit Emitter(const Module& module) : module_(module) {}

  constexpr SpirvEmitResult emit(SpirvSink& sink) {
    if (!module_.isValid()) return {SpirvEmitError::InvalidModule, {}};
    for (ArenaId i = 0; i < module_.functionCount; ++i) functionIds_[i] = id();
    declareBindings();
    declareInterfaces();
    for (ArenaId i = 0; i < module_.functionCount && result_.isSuccess(); ++i) emitFunction(i);
    if (!declarations_.valid || !annotations_.valid || !functions_.valid)
      fail(SpirvEmitError::Capacity);
    if (!result_.isSuccess()) return result_;
    writeModuleHeader(sink);
    writeEntryPoints(sink);
    if (!result_.isSuccess()) return result_;
    append(sink, annotations_);
    append(sink, declarations_);
    append(sink, functions_);
    if (sink.error != SpirvEmitError::None) return {sink.error, {}};
    return result_;
  }

private:
  static constexpr uint32_t resourceStorage(BindingKind kind) {
    switch (kind) {
      case BindingKind::Uniform: return 2;
      case BindingKind::ReadOnlyStorage: return 12;
      default: return 0;
    }
  }

  constexpr void declareBindings() {
    for (ArenaId i = 0; i < module_.bindingCount; ++i) {
      const Binding& binding = module_.bindings[i];
      const uint32_t storage = resourceStorage(binding.kind);
      const uint32_t blockType = binding.type.kind == TypeKind::Array ? arrayBlockType(binding.type)
                                                                      : typeId(binding.type);
      const uint32_t pointer = pointerType(blockType, storage);
      bindingIds_[i] = id();
      declarations_.instruction(59, pointer, bindingIds_[i], storage);
      annotations_.instruction(71, bindingIds_[i], 33, binding.binding);
      annotations_.instruction(71, bindingIds_[i], 34, binding.group);
      if (binding.kind == BindingKind::StorageTexture)
        annotations_.instruction(71, bindingIds_[i], 25);
      if (binding.kind == BindingKind::ReadOnlyStorage)
        annotations_.instruction(71, bindingIds_[i], 24);
    }
  }

  constexpr uint32_t arrayBlockType(Type type) {
    const uint32_t array = typeId(type);
    for (uint16_t i = 0; i < arrayBlockCount_; ++i)
      if (arrayBlocks_[i].type == array) return arrayBlocks_[i].id;
    if (arrayBlockCount_ == arrayBlocks_.size()) {
      fail(SpirvEmitError::Capacity);
      return 0;
    }
    const uint32_t block = id();
    arrayBlocks_[arrayBlockCount_++] = {array, 0, block};
    declarations_.instruction(30, block, array);
    annotations_.instruction(72, block, 0, 35, 0);
    annotations_.instruction(71, block, 2);
    return block;
  }

  constexpr bool validIoRange(uint16_t first, uint16_t count) {
    if (first > module_.interfaceVariableCount || count > module_.interfaceVariableCount - first) {
      fail(SpirvEmitError::InvalidNode);
      return false;
    }
    return true;
  }

  constexpr void declareInterfaceRange(uint16_t first, uint16_t count, Stage stage, bool input) {
    if (!validIoRange(first, count)) return;
    for (uint16_t i = first; i < first + count; ++i) {
      const InterfaceVariable& variable = module_.interfaceVariables[i];
      interfaceIds_[i] = id();
      const uint32_t storage = input ? 1 : 3;
      const uint32_t pointer = pointerType(typeId(variable.type), storage);
      declarations_.instruction(59, pointer, interfaceIds_[i], storage);
      if (variable.decoration.location != UINT32_MAX) {
        annotations_.instruction(71, interfaceIds_[i], 30, variable.decoration.location);
        if (variable.decoration.flat) annotations_.instruction(71, interfaceIds_[i], 14);
      } else {
        uint32_t builtin = 0;
        switch (variable.decoration.builtin) {
          case BuiltinValue::GlobalInvocationId: builtin = 28; break;
          case BuiltinValue::VertexIndex: builtin = 42; break;
          case BuiltinValue::InstanceIndex: builtin = 43; break;
          case BuiltinValue::Position: builtin = stage == Stage::Fragment ? 15 : 0; break;
          default: fail(SpirvEmitError::InvalidNode); return;
        }
        annotations_.instruction(71, interfaceIds_[i], 11, builtin);
      }
    }
  }

  constexpr void declareInterfaces() {
    for (ArenaId i = 0; i < module_.functionCount; ++i) {
      const Function& function = module_.functions[i];
      if (function.stage == Stage::None) continue;
      declareInterfaceRange(function.firstInput, function.inputCount, function.stage, true);
      declareInterfaceRange(function.firstOutput, function.outputCount, function.stage, false);
    }
  }

  constexpr void writeModuleHeader(SpirvSink& sink) {
    sink.word(0x07230203u);
    sink.word(0x00010300u);
    sink.word(0);
    sink.word(nextId_);
    sink.word(0);
    writeInstruction(sink, 17, 1);
    writeInstruction(sink, 17, 50);
    sink.word((6u << 16) | 11u);
    sink.word(1);
    Words<4> extension;
    extension.string("GLSL.std.450");
    for (uint32_t i = 0; i < extension.size; ++i) sink.word(extension.data[i]);
    writeInstruction(sink, 14, 0, 1);
  }

  constexpr void writeEntryPoint(SpirvSink& sink, const Function& function, ArenaId i) {
    Words<64> encoded;
    encoded.string(module_.name(function.name));
    if (!encoded.valid) {
      fail(SpirvEmitError::Capacity);
      return;
    }
    sink.word(((3u + encoded.size + function.inputCount + function.outputCount) << 16) | 15u);
    sink.word(function.stage == Stage::Compute ? 5 : function.stage == Stage::Vertex ? 0 : 4);
    sink.word(functionIds_[i]);
    for (uint32_t j = 0; j < encoded.size; ++j) sink.word(encoded.data[j]);
    for (uint16_t j = 0; j < function.inputCount; ++j)
      sink.word(interfaceIds_[function.firstInput + j]);
    for (uint16_t j = 0; j < function.outputCount; ++j)
      sink.word(interfaceIds_[function.firstOutput + j]);
  }

  constexpr void writeEntryPoints(SpirvSink& sink) {
    for (ArenaId i = 0; i < module_.functionCount; ++i) {
      const Function& function = module_.functions[i];
      if (function.stage == Stage::None) continue;
      writeEntryPoint(sink, function, i);
    }
    for (ArenaId i = 0; i < module_.functionCount; ++i) {
      const Function& function = module_.functions[i];
      if (function.stage == Stage::Compute)
        writeInstruction(sink, 16, functionIds_[i], 17, function.workgroupSize[0],
                         function.workgroupSize[1], function.workgroupSize[2]);
      if (function.stage == Stage::Fragment) writeInstruction(sink, 16, functionIds_[i], 7);
    }
  }

  constexpr void fail(SpirvEmitError error, SourceSpan span = {}) {
    if (result_.isSuccess()) result_ = {error, span};
  }
  constexpr uint32_t id() { return nextId_++; }

  constexpr uint32_t typeId(Type type) {
    if (type.kind == TypeKind::Pointer) return pointerType(typeId(type.elementType()), 7);
    for (uint32_t i = 0; i < typeCount_; ++i)
      if (types_[i].type == type) return types_[i].id;
    if (typeCount_ == types_.size()) {
      fail(SpirvEmitError::Capacity);
      return 0;
    }
    if (type.lanes < 1 || type.lanes > 4) {
      fail(SpirvEmitError::UnsupportedType);
      return 0;
    }
    const uint32_t value = id();
    types_[typeCount_++] = {type, value};
    declareType(type, value);
    return value;
  }

  constexpr void declareVectorType(Type type, uint32_t value) {
    if (!type.isNumeric() && type.kind != TypeKind::Bool) {
      fail(SpirvEmitError::UnsupportedType);
      return;
    }
    Type scalar = type;
    scalar.lanes = 1;
    declarations_.instruction(23, value, typeId(scalar), type.lanes);
  }

  constexpr void declareImageType(Type type, uint32_t value) {
    if (type.kind == TypeKind::StorageTexture2d && !type.hasSupportedStorageFormat()) {
      fail(SpirvEmitError::UnsupportedType);
      return;
    }
    declarations_.instruction(25, value, typeId(Type{TypeKind::F32}), 1, 0, 0, 0,
                              type.kind == TypeKind::SampledTexture2d ? 1 : 2,
                              type.kind == TypeKind::SampledTexture2d                  ? 0
                              : type.storageFormat == StorageTextureFormat::Rgba8Unorm ? 4
                                                                                       : 1);
  }

  constexpr void declareArrayType(Type type, uint32_t value) {
    const uint32_t stride = module_.arrayStride(type);
    if (stride == 0 || type.arrayCount > ModuleLimits::kMaxArrayElements) {
      fail(SpirvEmitError::UnsupportedType);
      return;
    }
    const uint32_t element = typeId(type.elementType());
    if (type.arrayCount == 0)
      declarations_.instruction(29, value, element);
    else
      declarations_.instruction(28, value, element, constant(Type{TypeKind::U32}, type.arrayCount));
    annotations_.instruction(71, value, 6, stride);
  }

  constexpr void declareStructType(Type type, uint32_t value) {
    if (type.structId >= module_.structCount) {
      fail(SpirvEmitError::InvalidNode);
      return;
    }
    const Struct& structure = module_.structs[type.structId];
    std::array<uint32_t, ModuleLimits::kMaxStructMembers> members{};
    for (uint16_t i = 0; i < structure.memberCount; ++i)
      members[i] = typeId(module_.structMembers[structure.firstMember + i].type);
    declarations_.word((uint32_t(structure.memberCount + 2) << 16) | 30u);
    declarations_.word(value);
    for (uint16_t i = 0; i < structure.memberCount; ++i) {
      declarations_.word(members[i]);
      // A structure reaching a bool has no defined buffer layout, so it carries no byte offsets.
      if (!structure.hostShareable) continue;
      annotations_.instruction(72, value, i, 35,
                               module_.structMembers[structure.firstMember + i].offset);
      const Type memberType = module_.structMembers[structure.firstMember + i].type;
      if (memberType.kind == TypeKind::Matrix) {
        annotations_.instruction(72, value, i, 5);
        annotations_.instruction(72, value, i, 7, memberType.rows == 2 ? 8 : 16);
      }
    }
    for (uint16_t i = 0; i < module_.bindingCount; ++i) {
      if (module_.bindings[i].type == type) {
        annotations_.instruction(71, value, 2);
        break;
      }
    }
  }

  constexpr void declareAggregateType(Type type, uint32_t value) {
    switch (type.kind) {
      case TypeKind::Sampler: declarations_.instruction(26, value); break;
      case TypeKind::SampledTexture2d:
      case TypeKind::StorageTexture2d: declareImageType(type, value); break;
      case TypeKind::Struct: declareStructType(type, value); break;
      case TypeKind::Array: declareArrayType(type, value); break;
      case TypeKind::Matrix:
        declarations_.instruction(24, value, typeId(Type{TypeKind::F32, type.rows}), type.columns);
        break;
      default: break;
    }
  }

  constexpr void declareType(Type type, uint32_t value) {
    if (type.lanes > 1) {
      declareVectorType(type, value);
      return;
    }
    switch (type.kind) {
      case TypeKind::AbstractInt:
      case TypeKind::AbstractFloat: fail(SpirvEmitError::UnsupportedType); break;
      case TypeKind::Void: declarations_.instruction(19, value); break;
      case TypeKind::Bool: declarations_.instruction(20, value); break;
      case TypeKind::I32: declarations_.instruction(21, value, 32, 1); break;
      case TypeKind::U32: declarations_.instruction(21, value, 32, 0); break;
      case TypeKind::F32: declarations_.instruction(22, value, 32); break;
      default: declareAggregateType(type, value); break;
    }
  }

  constexpr uint32_t pointerType(uint32_t type, uint32_t storage) {
    for (uint32_t i = 0; i < pointerCount_; ++i)
      if (pointers_[i].type == type && pointers_[i].storage == storage) return pointers_[i].id;
    if (pointerCount_ == pointers_.size()) {
      fail(SpirvEmitError::Capacity);
      return 0;
    }
    const uint32_t value = id();
    pointers_[pointerCount_++] = {type, storage, value};
    declarations_.instruction(32, value, storage, type);
    return value;
  }

  constexpr void declareConstant(Type type, uint32_t bits, uint32_t value) {
    const uint32_t typeValue = typeId(type);
    if (type.lanes > 1) {
      Type scalar = type;
      scalar.lanes = 1;
      const uint32_t component = constant(scalar, bits);
      declarations_.word((uint32_t(type.lanes + 3) << 16) | 44u);
      declarations_.word(typeValue);
      declarations_.word(value);
      for (uint8_t i = 0; i < type.lanes; ++i) declarations_.word(component);
    } else if (type.kind == TypeKind::Struct || type.kind == TypeKind::Matrix ||
               type.kind == TypeKind::Array) {
      declarations_.instruction(46, typeValue, value);
    } else if (type.kind == TypeKind::Bool) {
      declarations_.instruction(bits ? 41 : 42, typeValue, value);
    } else {
      declarations_.instruction(43, typeValue, value, bits);
    }
  }

  constexpr uint32_t constant(Type type, uint32_t bits) {
    for (uint32_t i = 0; i < constantCount_; ++i)
      if (constants_[i].type == type && constants_[i].bits == bits) return constants_[i].id;
    if (constantCount_ == constants_.size()) {
      fail(SpirvEmitError::Capacity);
      return 0;
    }
    const uint32_t value = id();
    constants_[constantCount_++] = {type, bits, value};
    declareConstant(type, bits, value);
    return value;
  }

  template <typename... Operands>
  constexpr uint32_t operation(uint32_t opcode, Type type, Operands... operands) {
    const uint32_t value = id();
    functions_.instruction(opcode, typeId(type), value, operands...);
    return value;
  }
  template <typename... Operands>
  constexpr uint32_t extended(uint32_t opcode, Type type, Operands... operands) {
    return operation(12, type, 1, opcode, operands...);
  }
  constexpr void label(uint32_t value) {
    functions_.instruction(248, value);
    currentBlock_ = value;
    terminated_ = false;
  }
  constexpr void branch(uint32_t value) {
    if (!terminated_) functions_.instruction(249, value);
    terminated_ = true;
  }
  constexpr uint32_t splat(Type type, uint32_t value) {
    if (type.lanes == 1) return value;
    const uint32_t result = id();
    functions_.word((uint32_t(type.lanes + 3) << 16) | 80u);
    functions_.word(typeId(type));
    functions_.word(result);
    for (uint8_t i = 0; i < type.lanes; ++i) functions_.word(value);
    return result;
  }
  constexpr uint32_t select(Type type, Type conditionType, uint32_t condition, uint32_t yes,
                            uint32_t no) {
    if (type.lanes > 1 && conditionType.lanes == 1)
      condition = splat(Type{TypeKind::Bool, type.lanes}, condition);
    return operation(169, type, condition, yes, no);
  }

  constexpr uint32_t functionType(const Function& function) {
    FunctionTypeRecord record;
    record.result = typeId(function.stage == Stage::None ? function.returnType : Type{});
    record.count = function.stage == Stage::None ? function.parameterCount : 0;
    if (record.count > record.parameters.size()) {
      fail(SpirvEmitError::Capacity);
      return 0;
    }
    for (uint16_t i = 0; i < record.count; ++i)
      record.parameters[i] = typeId(module_.symbols[function.firstParameter + i].type);
    for (uint16_t i = 0; i < functionTypeCount_; ++i) {
      const auto& other = functionTypes_[i];
      if (other.result == record.result && other.count == record.count &&
          other.parameters == record.parameters)
        return other.id;
    }
    record.id = id();
    functionTypes_[functionTypeCount_++] = record;
    declarations_.word((uint32_t(record.count + 3) << 16) | 33u);
    declarations_.word(record.id);
    declarations_.word(record.result);
    for (uint16_t i = 0; i < record.count; ++i) declarations_.word(record.parameters[i]);
    return record.id;
  }

  static constexpr bool localStorage(const Symbol& symbol) {
    return symbol.kind == SymbolKind::Var ||
           (symbol.kind == SymbolKind::Let && symbol.type.kind == TypeKind::Array);
  }

  constexpr uint32_t arrayTemporary(ArenaId expression) const {
    for (uint16_t i = 0; i < arrayTemporaryCount_; ++i)
      if (arrayTemporaries_[i].expression == expression) return arrayTemporaries_[i].pointer;
    return 0;
  }

  constexpr void declareArrayTemporaries(ArenaId expression, uint16_t depth = 0) {
    if (expression == kInvalidArenaId || !result_.isSuccess()) return;
    if (expression >= module_.expressionCount || depth >= 128) {
      fail(SpirvEmitError::InvalidNode);
      return;
    }
    const Expression& node = module_.expressions[expression];
    if (node.operandCount > node.operands.size()) {
      fail(SpirvEmitError::InvalidNode, node.span);
      return;
    }
    for (uint8_t i = 0; i < node.operandCount; ++i)
      declareArrayTemporaries(node.operands[i], depth + 1);
    declareIndexedTemporary(node);
  }

  constexpr void declareIndexedTemporary(const Expression& node) {
    if (node.kind != ExpressionKind::Index || node.operandCount != 2 ||
        node.operands[0] >= module_.expressionCount)
      return;
    const ArenaId base = node.operands[0];
    const Type type = module_.expressions[base].type;
    if (type.kind != TypeKind::Array || type.arrayCount == 0 || addressable(base) ||
        arrayTemporary(base))
      return;
    if (arrayTemporaryCount_ == arrayTemporaries_.size()) {
      fail(SpirvEmitError::Capacity, node.span);
      return;
    }
    const uint32_t pointerTypeId = pointerType(typeId(type), 7);
    const uint32_t pointer = id();
    arrayTemporaries_[arrayTemporaryCount_++] = {base, pointer};
    functions_.instruction(59, pointerTypeId, pointer, 7);
  }

  constexpr void declareVariables(ArenaId statement) {
    for (ArenaId current = statement; current != kInvalidArenaId && result_.isSuccess();
         current = module_.statements[current].next) {
      if (current >= module_.statementCount) {
        fail(SpirvEmitError::InvalidNode);
        return;
      }
      const Statement& node = module_.statements[current];
      if (node.kind == StatementKind::Declaration && node.symbolId < module_.symbolCount &&
          localStorage(module_.symbols[node.symbolId])) {
        const uint32_t pointer = pointerType(typeId(module_.symbols[node.symbolId].type), 7);
        symbolValues_[node.symbolId] = id();
        functions_.instruction(59, pointer, symbolValues_[node.symbolId], 7);
      }
      if (module_.functions[currentFunction_].hasLocalArrays) {
        declareArrayTemporaries(node.expression);
        declareArrayTemporaries(node.secondExpression);
        declareArrayTemporaries(node.thirdExpression);
      }
      if (node.init != kInvalidArenaId) declareVariables(node.init);
      if (node.firstBody != kInvalidArenaId) declareVariables(node.firstBody);
      if (node.firstElseBody != kInvalidArenaId) declareVariables(node.firstElseBody);
    }
  }

  constexpr void emitFunction(ArenaId index) {
    const Function& function = module_.functions[index];
    currentFunction_ = index;
    arrayTemporaryCount_ = 0;
    for (uint16_t i = 0; i < module_.symbolCount; ++i) symbolValues_[i] = 0;
    const uint32_t signature = functionType(function);
    functions_.instruction(54, typeId(function.stage == Stage::None ? function.returnType : Type{}),
                           functionIds_[index], 0, signature);
    if (function.stage == Stage::None) {
      for (uint16_t i = 0; i < function.parameterCount; ++i) {
        const ArenaId symbol = function.firstParameter + i;
        symbolValues_[symbol] = id();
        functions_.instruction(55, typeId(module_.symbols[symbol].type), symbolValues_[symbol]);
      }
    }
    label(id());
    declareVariables(function.firstStatement);
    if (function.stage != Stage::None) loadEntryParameters(function);
    emitBlock(function.firstStatement);
    if (!terminated_) {
      if (function.returnType.kind != TypeKind::Void)
        fail(SpirvEmitError::InvalidNode);
      else
        functions_.instruction(253);
    }
    functions_.instruction(56);
  }

  constexpr void loadEntryParameters(const Function& function) {
    for (uint16_t parameter = 0; parameter < function.parameterCount; ++parameter) {
      const ArenaId symbol = function.firstParameter + parameter;
      const Type type = module_.symbols[symbol].type;
      std::array<uint32_t, ModuleLimits::kMaxStructMembers> values{};
      uint16_t count = 0;
      for (uint16_t i = function.firstInput; i < function.firstInput + function.inputCount; ++i) {
        const InterfaceVariable& variable = module_.interfaceVariables[i];
        if (variable.symbol == symbol)
          values[count++] = operation(61, variable.type, interfaceIds_[i]);
      }
      if (type.kind != TypeKind::Struct) {
        if (count != 1) {
          fail(SpirvEmitError::InvalidNode);
          return;
        }
        symbolValues_[symbol] = values[0];
      } else {
        const uint32_t result = id();
        functions_.word((uint32_t(count + 3) << 16) | 80u);
        functions_.word(typeId(type));
        functions_.word(result);
        for (uint16_t i = 0; i < count; ++i) functions_.word(values[i]);
        symbolValues_[symbol] = result;
      }
    }
  }

  constexpr void emitReturn(ArenaId expression) {
    const Function& function = module_.functions[currentFunction_];
    if (expression == kInvalidArenaId) {
      functions_.instruction(253);
      return;
    }
    const uint32_t value = emitExpression(expression);
    if (function.stage == Stage::None) {
      functions_.instruction(254, value);
      return;
    }
    for (uint16_t i = function.firstOutput; i < function.firstOutput + function.outputCount; ++i) {
      const InterfaceVariable& variable = module_.interfaceVariables[i];
      uint32_t leaf = value;
      if (variable.member != kInvalidArenaId) {
        const uint32_t member =
            variable.member - module_.structs[function.returnType.structId].firstMember;
        leaf = operation(81, variable.type, value, member);
      }
      functions_.instruction(62, interfaceIds_[i], leaf);
    }
    functions_.instruction(253);
  }

  constexpr bool addressable(ArenaId expression) const {
    for (uint16_t depth = 0; depth < ModuleLimits::kMaxNesting; ++depth) {
      if (expression >= module_.expressionCount) return false;
      const Expression& node = module_.expressions[expression];
      if (node.kind == ExpressionKind::Deref) return true;
      if (node.kind == ExpressionKind::Symbol && node.payload < module_.symbolCount) {
        const auto& symbol = module_.symbols[node.payload];
        return localStorage(symbol) || symbol.kind == SymbolKind::Binding;
      }
      if (node.kind == ExpressionKind::Index && node.operands[0] < module_.expressionCount) {
        const Type base = module_.expressions[node.operands[0]].type;
        if (base.kind == TypeKind::Array && base.arrayCount == 0) return false;
      }
      if (node.kind != ExpressionKind::Member && node.kind != ExpressionKind::Index) return false;
      expression = node.operands[0];
    }
    return false;
  }

  constexpr uint32_t lvalue(ArenaId expression, uint32_t& storage) {
    if (expression >= module_.expressionCount) {
      fail(SpirvEmitError::InvalidNode);
      return 0;
    }
    const Expression& node = module_.expressions[expression];
    if (node.kind == ExpressionKind::Symbol && node.payload < module_.symbolCount) {
      const Symbol& symbol = module_.symbols[node.payload];
      if (symbol.kind == SymbolKind::Binding && symbol.bindingId < module_.bindingCount) {
        storage = resourceStorage(module_.bindings[symbol.bindingId].kind);
        return bindingIds_[symbol.bindingId];
      }
      if (localStorage(symbol)) {
        storage = 7;
        return symbolValues_[node.payload];
      }
    }
    if (node.kind == ExpressionKind::Deref) return pointerValue(node.operands[0], storage);
    if (node.kind == ExpressionKind::Index) return indexedPointer(node, storage);
    if (node.kind == ExpressionKind::Member && node.operandCount == 1) {
      const auto& base = module_.expressions[node.operands[0]];
      if (base.type.structId >= module_.structCount) {
        fail(SpirvEmitError::InvalidNode);
        return 0;
      }
      const uint32_t basePointer = lvalue(node.operands[0], storage);
      const uint32_t member = node.payload - module_.structs[base.type.structId].firstMember;
      const uint32_t pointer = id();
      functions_.instruction(65, pointerType(typeId(node.type), storage), pointer, basePointer,
                             constant(Type{TypeKind::U32}, member));
      return pointer;
    }
    if (node.kind == ExpressionKind::Swizzle && node.type.lanes == 1) {
      const uint32_t basePointer = lvalue(node.operands[0], storage);
      const uint32_t pointer = id();
      functions_.instruction(65, pointerType(typeId(node.type), storage), pointer, basePointer,
                             constant(Type{TypeKind::U32}, node.payload & 3u));
      return pointer;
    }
    fail(SpirvEmitError::InvalidNode, node.span);
    return 0;
  }

  /// Returns the SPIR-V pointer produced by a pointer-typed expression.
  /// @param expression `&local` or a pointer parameter reference.
  /// @param storage Receives the SPIR-V storage class of the result.
  constexpr uint32_t pointerValue(ArenaId expression, uint32_t& storage) {
    if (expression >= module_.expressionCount) {
      fail(SpirvEmitError::InvalidNode);
      return 0;
    }
    const Expression& node = module_.expressions[expression];
    if (node.kind == ExpressionKind::AddressOf && node.operandCount == 1) {
      const uint32_t pointer = lvalue(node.operands[0], storage);
      if (storage != 7) fail(SpirvEmitError::InvalidNode, node.span);
      return pointer;
    }
    if (node.kind == ExpressionKind::Symbol && node.payload < module_.symbolCount &&
        module_.symbols[node.payload].type.kind == TypeKind::Pointer) {
      storage = 7;
      return symbolValues_[node.payload];
    }
    fail(SpirvEmitError::InvalidNode, node.span);
    return 0;
  }

  constexpr uint32_t emitExpression(ArenaId index);
  constexpr uint32_t arrayPointer(ArenaId expression, uint32_t& storage, SourceSpan span);
  constexpr uint32_t indexedPointer(const Expression& node, uint32_t& storage);
  constexpr uint32_t emitValue(const Expression& node);
  constexpr uint32_t emitPointerOperator(const Expression& node);
  constexpr uint32_t emitUnary(const Expression& node);
  constexpr uint32_t emitAccess(ArenaId index, const Expression& node);
  constexpr uint32_t emitSymbol(const Expression& node);
  constexpr uint32_t emitSwizzle(const Expression& node);
  constexpr uint32_t emitConstruct(const Expression& node);
  constexpr uint32_t emitRuntimeArrayIndex(const Expression& node);
  constexpr uint32_t emitCall(const Expression& node);
  constexpr uint32_t emitShortCircuit(const Expression& node, uint32_t lhs);
  constexpr uint32_t safeDivisor(Type type, uint32_t lhs, uint32_t rhs);
  constexpr uint32_t textureDimensions(const Expression& node, uint32_t image);
  constexpr void emitDeclaration(const Statement& node);
  constexpr void emitAssignment(const Statement& node);
  constexpr void emitIf(const Statement& node);
  constexpr void emitSwitchSelectors(
      const std::array<ArenaId, ModuleLimits::kMaxStatements>& clauses,
      const std::array<uint32_t, ModuleLimits::kMaxStatements>& labels, uint16_t count,
      SourceSpan span);
  constexpr void emitSwitch(const Statement& node);
  constexpr void emitFor(const Statement& node);
  constexpr void emitWhile(const Statement& node);
  constexpr void emitLoop(const Statement& node);
  constexpr uint32_t convert(Type from, Type to, uint32_t operand);
  constexpr uint32_t emitBinary(const Expression& expression);
  constexpr uint32_t emitMix(const Expression& node, std::array<uint32_t, 4> args);
  constexpr uint32_t emitBuiltin(const Expression& expression);
  constexpr uint32_t emitSampling(const Expression& node, const std::array<uint32_t, 4>& args);
  constexpr uint32_t emitMatrixProduct(const Expression& node, Type leftType, Type rightType,
                                       uint32_t lhs, uint32_t rhs);
  constexpr uint32_t emitNumericBinary(const Expression& node, Type leftType, Type rightType,
                                       uint32_t lhs, uint32_t rhs);
  static constexpr uint32_t UnaryBuiltinOpcode(Builtin builtin);
  constexpr uint32_t emitCoreBuiltin(const Expression& node, const std::array<uint32_t, 4>& args);
  constexpr void emitLoopExit(const Statement& node);
  constexpr uint32_t robustLoad(uint32_t image, uint32_t coord, uint32_t level);
  constexpr void robustStore(uint32_t image, uint32_t coord, uint32_t value);
  constexpr void emitBlock(ArenaId index);
  constexpr void emitControlStatement(const Statement& node);
  constexpr void emitStatement(const Statement& statement);

  const Module& module_;
  SpirvEmitResult result_;
  uint32_t nextId_ = 2;
  uint32_t currentBlock_ = 0;
  uint32_t sampledImageType_ = 0;
  ArenaId currentFunction_ = kInvalidArenaId;
  bool terminated_ = false;
  uint32_t breakTarget_ = 0;
  uint32_t continueTarget_ = 0;
  uint16_t expressionDepth_ = 0;
  Words<2048> annotations_;
  Words<4096> declarations_;
  Words<32768> functions_;
  std::array<TypeRecord, 64> types_{};
  uint16_t typeCount_ = 0;
  std::array<PointerRecord, 96> pointers_{};
  uint16_t pointerCount_ = 0;
  std::array<PointerRecord, ModuleLimits::kMaxBindings> arrayBlocks_{};
  uint16_t arrayBlockCount_ = 0;
  std::array<ConstantRecord, 128> constants_{};
  uint16_t constantCount_ = 0;
  std::array<FunctionTypeRecord, ModuleLimits::kMaxFunctions> functionTypes_{};
  uint16_t functionTypeCount_ = 0;
  std::array<uint32_t, ModuleLimits::kMaxBindings> bindingIds_{};
  std::array<uint32_t, ModuleLimits::kMaxFunctions> functionIds_{};
  std::array<uint32_t, ModuleLimits::kMaxInterfaceVariables> interfaceIds_{};
  std::array<uint32_t, ModuleLimits::kMaxSymbols> symbolValues_{};
  std::array<ArrayTemporary, ModuleLimits::kMaxStatements> arrayTemporaries_{};
  uint16_t arrayTemporaryCount_ = 0;
};

constexpr uint32_t Emitter::arrayPointer(ArenaId expression, uint32_t& storage, SourceSpan span) {
  uint32_t base = 0;
  if (addressable(expression))
    base = lvalue(expression, storage);
  else {
    base = arrayTemporary(expression);
    if (!base) {
      fail(SpirvEmitError::InvalidNode, span);
      return 0;
    }
    const uint32_t value = emitExpression(expression);
    functions_.instruction(62, base, value);
    storage = 7;
  }
  return base;
}

constexpr uint32_t Emitter::indexedPointer(const Expression& node, uint32_t& storage) {
  if (node.operandCount != 2 || node.operands[0] >= module_.expressionCount ||
      node.operands[1] >= module_.expressionCount) {
    fail(SpirvEmitError::InvalidNode, node.span);
    return 0;
  }
  const Type array = module_.expressions[node.operands[0]].type;
  const Type indexType = module_.expressions[node.operands[1]].type;
  if (array.kind != TypeKind::Array || array.arrayCount == 0 || indexType.lanes != 1 ||
      (indexType.kind != TypeKind::I32 && indexType.kind != TypeKind::U32)) {
    fail(SpirvEmitError::InvalidNode, node.span);
    return 0;
  }
  const uint32_t base = arrayPointer(node.operands[0], storage, node.span);
  const uint32_t index = emitExpression(node.operands[1]);
  const uint32_t bounded =
      extended(indexType.kind == TypeKind::I32 ? 45 : 44, indexType, index, constant(indexType, 0),
               constant(indexType, array.arrayCount - 1));
  const uint32_t result = id();
  functions_.instruction(65, pointerType(typeId(node.type), storage), result, base, bounded);
  return result;
}

constexpr uint32_t Emitter::emitRuntimeArrayIndex(const Expression& node) {
  const Expression& base = module_.expressions[node.operands[0]];
  if (base.kind != ExpressionKind::Symbol || base.payload >= module_.symbolCount) {
    fail(SpirvEmitError::InvalidNode);
    return 0;
  }
  const Symbol& symbol = module_.symbols[base.payload];
  if (symbol.kind != SymbolKind::Binding || symbol.bindingId >= module_.bindingCount) {
    fail(SpirvEmitError::InvalidNode);
    return 0;
  }
  uint32_t index = emitExpression(node.operands[1]);
  const Type indexType = module_.expressions[node.operands[1]].type;
  const Type integer{TypeKind::U32};
  if (indexType.kind == TypeKind::I32) {
    index = extended(42, indexType, index, constant(indexType, 0));
    index = operation(124, integer, index);
  }
  const uint32_t count = operation(68, integer, bindingIds_[symbol.bindingId], 0);
  const uint32_t nonempty = operation(171, Type{TypeKind::Bool}, count, constant(integer, 0));
  const uint32_t readLabel = id(), emptyLabel = id(), mergeLabel = id();
  functions_.instruction(247, mergeLabel, 0);
  functions_.instruction(250, nonempty, readLabel, emptyLabel);
  label(readLabel);
  const uint32_t last = operation(130, integer, count, constant(integer, 1));
  const uint32_t bounded = extended(38, integer, index, last);
  const uint32_t pointer = id();
  functions_.instruction(65, pointerType(typeId(node.type), 12), pointer,
                         bindingIds_[symbol.bindingId], constant(integer, 0), bounded);
  const uint32_t loaded = operation(61, node.type, pointer);
  const uint32_t readBlock = currentBlock_;
  branch(mergeLabel);
  label(emptyLabel);
  const uint32_t zero = constant(node.type, 0);
  const uint32_t emptyBlock = currentBlock_;
  branch(mergeLabel);
  label(mergeLabel);
  return operation(245, node.type, loaded, readBlock, zero, emptyBlock);
}

constexpr uint32_t Emitter::emitExpression(ArenaId index) {
  if (index >= module_.expressionCount) {
    fail(SpirvEmitError::InvalidNode);
    return 0;
  }
  if (++expressionDepth_ > 128) {
    fail(SpirvEmitError::InvalidNode);
    --expressionDepth_;
    return 0;
  }
  const Expression& node = module_.expressions[index];
  uint32_t value = 0;
  switch (node.kind) {
    case ExpressionKind::Zero: value = constant(node.type, 0); break;
    case ExpressionKind::Literal: value = constant(node.type, node.payload); break;
    case ExpressionKind::Symbol: value = emitSymbol(node); break;
    case ExpressionKind::Member:
    case ExpressionKind::Index: value = emitAccess(index, node); break;
    default: value = emitValue(node); break;
  }
  --expressionDepth_;
  return value;
}

constexpr uint32_t Emitter::emitAccess(ArenaId index, const Expression& node) {
  const Type base = module_.expressions[node.operands[0]].type;
  if (node.kind == ExpressionKind::Member && !addressable(index)) {
    if (base.structId >= module_.structCount) {
      fail(SpirvEmitError::InvalidNode);
      return 0;
    }
    const uint32_t member = node.payload - module_.structs[base.structId].firstMember;
    return operation(81, node.type, emitExpression(node.operands[0]), member);
  }
  if (base.kind == TypeKind::Matrix) {
    if (node.payload >= base.columns) {
      fail(SpirvEmitError::InvalidNode);
      return 0;
    }
    return operation(81, node.type, emitExpression(node.operands[0]), node.payload);
  }
  if (base.kind == TypeKind::Array && base.arrayCount == 0) return emitRuntimeArrayIndex(node);
  uint32_t storage = 0;
  return operation(61, node.type, lvalue(index, storage));
}

constexpr uint32_t Emitter::emitPointerOperator(const Expression& node) {
  uint32_t storage = 0;
  if (node.kind == ExpressionKind::Deref)
    return operation(61, node.type, pointerValue(node.operands[0], storage));
  const uint32_t pointer = lvalue(node.operands[0], storage);
  if (storage != 7) {
    fail(SpirvEmitError::InvalidNode, node.span);
    return 0;
  }
  return pointer;
}

constexpr uint32_t Emitter::emitUnary(const Expression& node) {
  const uint32_t operand = emitExpression(node.operands[0]);
  if (static_cast<UnaryOp>(node.payload) == UnaryOp::Not) return operation(168, node.type, operand);
  return operation(node.type.kind == TypeKind::F32 ? 127 : 126, node.type, operand);
}

constexpr uint32_t Emitter::emitValue(const Expression& node) {
  switch (node.kind) {
    default: break;
    case ExpressionKind::AddressOf:
    case ExpressionKind::Deref: return emitPointerOperator(node);
    case ExpressionKind::Swizzle: return emitSwizzle(node);
    case ExpressionKind::Unary: return emitUnary(node);
    case ExpressionKind::Binary: return emitBinary(node);
    case ExpressionKind::Construct: return emitConstruct(node);
    case ExpressionKind::Convert: {
      const uint32_t operand = emitExpression(node.operands[0]);
      const Type from = module_.expressions[node.operands[0]].type;
      return convert(from, node.type, operand);
    }
    case ExpressionKind::BuiltinCall: return emitBuiltin(node);
    case ExpressionKind::FunctionCall: return emitCall(node);
  }
  fail(SpirvEmitError::InvalidNode, node.span);
  return 0;
}

constexpr uint32_t Emitter::emitSymbol(const Expression& node) {
  if (node.payload >= module_.symbolCount) {
    fail(SpirvEmitError::InvalidNode);
    return 0;
  }
  const Symbol& symbol = module_.symbols[node.payload];
  if (symbol.kind == SymbolKind::Binding) {
    if (symbol.type.kind == TypeKind::Struct || symbol.type.kind == TypeKind::Array ||
        symbol.bindingId >= module_.bindingCount) {
      fail(SpirvEmitError::UnsupportedType, node.span);
      return 0;
    }
    return operation(61, node.type, bindingIds_[symbol.bindingId]);
  }
  if (localStorage(symbol)) return operation(61, node.type, symbolValues_[node.payload]);
  if (!symbolValues_[node.payload]) fail(SpirvEmitError::InvalidNode, node.span);
  return symbolValues_[node.payload];
}

constexpr uint32_t Emitter::emitSwizzle(const Expression& node) {
  const uint32_t base = emitExpression(node.operands[0]);
  if (node.type.lanes == 1) return operation(81, node.type, base, node.payload & 3u);
  const uint32_t result = id();
  functions_.word((uint32_t(node.type.lanes + 5) << 16) | 79u);
  functions_.word(typeId(node.type));
  functions_.word(result);
  functions_.word(base);
  functions_.word(base);
  for (uint8_t lane = 0; lane < node.type.lanes; ++lane)
    functions_.word((node.payload >> (2 * lane)) & 3u);
  return result;
}

constexpr uint32_t Emitter::emitConstruct(const Expression& node) {
  std::array<uint32_t, Expression::kMaxOperands> values{};
  for (uint8_t i = 0; i < node.operandCount; ++i) values[i] = emitExpression(node.operands[i]);
  if (node.type.kind == TypeKind::Matrix && node.operandCount == 1) return values[0];
  const bool numericConversion = node.type.isNumeric() && node.operandCount == 1;
  if (numericConversion && module_.expressions[node.operands[0]].type.lanes == 1)
    return splat(node.type, values[0]);
  if (numericConversion)
    return convert(module_.expressions[node.operands[0]].type, node.type, values[0]);
  const uint32_t result = id();
  functions_.word((uint32_t(node.operandCount + 3) << 16) | 80u);
  functions_.word(typeId(node.type));
  functions_.word(result);
  for (uint8_t i = 0; i < node.operandCount; ++i) functions_.word(values[i]);
  return result;
}

constexpr uint32_t Emitter::emitCall(const Expression& node) {
  if (node.payload >= module_.functionCount || node.operandCount > node.operands.size()) {
    fail(SpirvEmitError::InvalidNode);
    return 0;
  }
  std::array<uint32_t, Expression::kMaxOperands> arguments{};
  for (uint8_t i = 0; i < node.operandCount; ++i) arguments[i] = emitExpression(node.operands[i]);
  const uint32_t result = id();
  functions_.word((uint32_t(node.operandCount + 4) << 16) | 57u);
  functions_.word(typeId(node.type));
  functions_.word(result);
  functions_.word(functionIds_[node.payload]);
  for (uint8_t i = 0; i < node.operandCount; ++i) functions_.word(arguments[i]);
  return result;
}

constexpr uint32_t Emitter::convert(Type from, Type to, uint32_t operand) {
  if (from == to) return operand;
  if (from.lanes != to.lanes || !from.isNumeric() || !to.isNumeric()) {
    fail(SpirvEmitError::UnsupportedType);
    return 0;
  }
  if (from.kind == TypeKind::F32) {
    const uint32_t low = constant(from, to.kind == TypeKind::I32 ? 0xcf000000u : 0u);
    const uint32_t high = constant(from, to.kind == TypeKind::I32 ? 0x4effffffu : 0x4f7fffffu);
    const Type boolean{TypeKind::Bool, from.lanes};
    const uint32_t ordered = operation(180, boolean, operand, operand);
    operand = select(from, boolean, ordered, operand, constant(from, 0));
    operand = extended(43, from, operand, low, high);
    return operation(to.kind == TypeKind::I32 ? 110 : 109, to, operand);
  }
  if (to.kind == TypeKind::F32)
    return operation(from.kind == TypeKind::I32 ? 111 : 112, to, operand);
  return operation(124, to, operand);
}

/// SPIR-V opcodes differ by scalar category; logical operators use control flow instead.
constexpr uint32_t BinaryOpcode(BinaryOp op, TypeKind kind) {
  struct Encoding {
    BinaryOp op;
    uint32_t floating;
    uint32_t signedInteger;
    uint32_t unsignedInteger;
    uint32_t boolean;
  };
  constexpr std::array<Encoding, 11> encodings{{
      {BinaryOp::Add, 129, 128, 128, 0},
      {BinaryOp::Sub, 131, 130, 130, 0},
      {BinaryOp::Mul, 133, 132, 132, 0},
      {BinaryOp::Div, 136, 135, 134, 0},
      {BinaryOp::Mod, 0, 138, 137, 0},
      {BinaryOp::Lt, 184, 177, 176, 0},
      {BinaryOp::Le, 188, 179, 178, 0},
      {BinaryOp::Gt, 186, 173, 172, 0},
      {BinaryOp::Ge, 190, 175, 174, 0},
      {BinaryOp::Eq, 180, 170, 170, 164},
      {BinaryOp::Ne, 183, 171, 171, 165},
  }};
  for (const Encoding& encoding : encodings) {
    if (encoding.op != op) continue;
    switch (kind) {
      case TypeKind::F32: return encoding.floating;
      case TypeKind::I32: return encoding.signedInteger;
      case TypeKind::U32: return encoding.unsignedInteger;
      case TypeKind::Bool: return encoding.boolean;
      default: return 0;
    }
  }
  return 0;
}

constexpr uint32_t NumericOpcode(TypeKind kind, uint32_t floating, uint32_t signedInteger,
                                 uint32_t unsignedInteger) {
  switch (kind) {
    case TypeKind::F32: return floating;
    case TypeKind::I32: return signedInteger;
    default: return unsignedInteger;
  }
}

constexpr uint32_t Emitter::emitShortCircuit(const Expression& node, uint32_t lhs) {
  const BinaryOp op = static_cast<BinaryOp>(node.payload);

  const uint32_t predecessor = currentBlock_;
  const uint32_t rhsBlock = id(), merge = id();
  functions_.instruction(247, merge, 0);
  functions_.instruction(250, lhs, op == BinaryOp::And ? rhsBlock : merge,
                         op == BinaryOp::And ? merge : rhsBlock);
  label(rhsBlock);
  const uint32_t rhs = emitExpression(node.operands[1]);
  const uint32_t rhsPredecessor = currentBlock_;
  branch(merge);
  label(merge);
  return operation(245, Type{TypeKind::Bool}, lhs, predecessor, rhs, rhsPredecessor);
}

constexpr uint32_t Emitter::safeDivisor(Type type, uint32_t lhs, uint32_t rhs) {
  const Type boolType{TypeKind::Bool, type.lanes};
  uint32_t unsafe = operation(170, boolType, rhs, constant(type, 0));
  if (type.kind == TypeKind::I32) {
    const uint32_t minimum = operation(170, boolType, lhs, constant(type, 0x80000000u));
    const uint32_t negativeOne = operation(170, boolType, rhs, constant(type, 0xffffffffu));
    unsafe = operation(166, boolType, unsafe, operation(167, boolType, minimum, negativeOne));
  }
  rhs = select(type, boolType, unsafe, constant(type, 1), rhs);
  return rhs;
}

constexpr uint32_t Emitter::emitBinary(const Expression& node) {
  const BinaryOp op = static_cast<BinaryOp>(node.payload);
  uint32_t lhs = emitExpression(node.operands[0]);
  if (op == BinaryOp::And || op == BinaryOp::Or) return emitShortCircuit(node, lhs);
  uint32_t rhs = emitExpression(node.operands[1]);
  if (op == BinaryOp::BitAnd) return operation(199, node.type, lhs, rhs);
  const Type leftType = module_.expressions[node.operands[0]].type;
  const Type rightType = module_.expressions[node.operands[1]].type;
  if (leftType.kind == TypeKind::Matrix || rightType.kind == TypeKind::Matrix)
    return emitMatrixProduct(node, leftType, rightType, lhs, rhs);
  return emitNumericBinary(node, leftType, rightType, lhs, rhs);
}

constexpr uint32_t Emitter::emitMatrixProduct(const Expression& node, Type leftType, Type rightType,
                                              uint32_t lhs, uint32_t rhs) {
  if (static_cast<BinaryOp>(node.payload) != BinaryOp::Mul) {
    fail(SpirvEmitError::InvalidNode);
    return 0;
  }
  if (leftType.kind == TypeKind::Matrix && rightType.kind == TypeKind::Matrix)
    return operation(146, node.type, lhs, rhs);
  if (leftType.kind == TypeKind::Matrix)
    return operation(rightType.lanes == 1 ? 143 : 145, node.type, lhs, rhs);
  return leftType.lanes == 1 ? operation(143, node.type, rhs, lhs)
                             : operation(144, node.type, lhs, rhs);
}

constexpr uint32_t Emitter::emitNumericBinary(const Expression& node, Type leftType, Type rightType,
                                              uint32_t lhs, uint32_t rhs) {
  const BinaryOp op = static_cast<BinaryOp>(node.payload);
  const TypeKind kind = leftType.kind;
  Type operandType = leftType;
  operandType.lanes = leftType.lanes > rightType.lanes ? leftType.lanes : rightType.lanes;
  if (leftType.lanes == 1) lhs = splat(operandType, lhs);
  if (rightType.lanes == 1) rhs = splat(operandType, rhs);
  if ((op == BinaryOp::Div || op == BinaryOp::Mod) && kind != TypeKind::F32)
    rhs = safeDivisor(operandType, lhs, rhs);
  const uint32_t opcode = BinaryOpcode(op, kind);
  if (!opcode) {
    fail(SpirvEmitError::InvalidNode, node.span);
    return 0;
  }
  return operation(opcode, node.type, lhs, rhs);
}

constexpr uint32_t Emitter::UnaryBuiltinOpcode(Builtin builtin) {
  struct Encoding {
    Builtin builtin;
    uint32_t opcode;
  };
  constexpr Encoding kEncodings[] = {
      {Builtin::Abs, 4},        {Builtin::Round, 2},  {Builtin::Sqrt, 31}, {Builtin::Length, 66},
      {Builtin::Normalize, 69}, {Builtin::Fract, 10}, {Builtin::Ceil, 9},  {Builtin::Exp, 27},
      {Builtin::Floor, 8},      {Builtin::Sign, 6},   {Builtin::Sin, 13},  {Builtin::Cos, 14},
  };
  for (const Encoding& encoding : kEncodings) {
    if (encoding.builtin == builtin) return encoding.opcode;
  }
  return 0;
}

constexpr uint32_t Emitter::emitSampling(const Expression& node,
                                         const std::array<uint32_t, 4>& args) {
  if (!sampledImageType_) {
    const uint32_t image = typeId(Type{TypeKind::SampledTexture2d});
    sampledImageType_ = id();
    declarations_.instruction(27, sampledImageType_, image);
  }
  const uint32_t sampled = id();
  functions_.instruction(86, sampledImageType_, sampled, args[0], args[1]);
  if (node.payload == uint32_t(Builtin::TextureSampleLevel))
    return operation(88, node.type, sampled, args[2], 2u, args[3]);
  return operation(87, node.type, sampled, args[2]);
}

constexpr uint32_t Emitter::emitMix(const Expression& node, std::array<uint32_t, 4> args) {
  if (module_.expressions[node.operands[2]].type.lanes == 1 && node.type.lanes > 1)
    args[2] = splat(node.type, args[2]);
  return extended(46, node.type, args[0], args[1], args[2]);
}

constexpr uint32_t Emitter::emitBuiltin(const Expression& node) {
  std::array<uint32_t, 4> args{};
  for (uint8_t i = 0; i < node.operandCount; ++i) args[i] = emitExpression(node.operands[i]);
  const Builtin builtin = static_cast<Builtin>(node.payload);
  if (const uint32_t opcode = UnaryBuiltinOpcode(builtin))
    return extended(opcode, node.type, args[0]);
  switch (builtin) {
    case Builtin::Pow: return extended(26, node.type, args[0], args[1]);
    case Builtin::TextureSample:
    case Builtin::TextureSampleLevel: return emitSampling(node, args);
    case Builtin::Mix: return emitMix(node, args);
    case Builtin::Max:
      return extended(NumericOpcode(node.type.kind, 40, 42, 41), node.type, args[0], args[1]);
    case Builtin::Min:
      return extended(NumericOpcode(node.type.kind, 37, 39, 38), node.type, args[0], args[1]);
    case Builtin::Clamp:
      return extended(NumericOpcode(node.type.kind, 43, 45, 44), node.type, args[0], args[1],
                      args[2]);
    case Builtin::Saturate:
      return extended(43, node.type, args[0], constant(node.type, 0),
                      constant(node.type, 0x3f800000u));
    default: return emitCoreBuiltin(node, args);
  }
}

constexpr uint32_t Emitter::emitCoreBuiltin(const Expression& node,
                                            const std::array<uint32_t, 4>& args) {
  switch (static_cast<Builtin>(node.payload)) {
    case Builtin::All: return operation(155, node.type, args[0]);
    case Builtin::Any: return operation(154, node.type, args[0]);
    case Builtin::Dot: return operation(148, node.type, args[0], args[1]);
    case Builtin::Fwidth: return operation(209, node.type, args[0]);
    case Builtin::Select:
      return select(node.type, module_.expressions[node.operands[2]].type, args[2], args[1],
                    args[0]);
    case Builtin::TextureLoad: return robustLoad(args[0], args[1], args[2]);
    case Builtin::TextureDimensions: return textureDimensions(node, args[0]);
    default: break;
  }
  fail(SpirvEmitError::InvalidNode, node.span);
  return 0;
}

constexpr uint32_t Emitter::textureDimensions(const Expression& node, uint32_t image) {
  if (module_.expressions[node.operands[0]].type.kind == TypeKind::StorageTexture2d)
    return operation(104, node.type, image);
  return operation(103, node.type, image, constant(Type{TypeKind::I32}, 0));
}

constexpr uint32_t Emitter::robustLoad(uint32_t image, uint32_t coord, uint32_t level) {
  const Type integer{TypeKind::I32}, integer2{TypeKind::I32, 2};
  const Type boolean{TypeKind::Bool}, boolean2{TypeKind::Bool, 2}, color{TypeKind::F32, 4};
  const uint32_t levels = operation(106, integer, image);
  const uint32_t nonnegative = operation(175, boolean, level, constant(integer, 0));
  const uint32_t inLevelRange = operation(177, boolean, level, levels);
  const uint32_t validLevel = operation(167, boolean, nonnegative, inLevelRange);
  const uint32_t safeLevel = select(integer, boolean, validLevel, level, constant(integer, 0));
  const uint32_t extent = operation(103, integer2, image, safeLevel);
  const uint32_t below =
      operation(154, boolean, operation(177, boolean2, coord, constant(integer2, 0)));
  const uint32_t above = operation(154, boolean, operation(175, boolean2, coord, extent));
  const uint32_t validCoord = operation(168, boolean, operation(166, boolean, below, above));
  const uint32_t condition = operation(167, boolean, validLevel, validCoord);
  const uint32_t predecessor = currentBlock_, loadBlock = id(), merge = id();
  functions_.instruction(247, merge, 0);
  functions_.instruction(250, condition, loadBlock, merge);
  label(loadBlock);
  const uint32_t loaded = operation(95, color, image, coord, 2, safeLevel);
  branch(merge);
  label(merge);
  return operation(245, color, constant(color, 0), predecessor, loaded, loadBlock);
}

constexpr void Emitter::robustStore(uint32_t image, uint32_t coord, uint32_t value) {
  const Type integer2{TypeKind::I32, 2}, boolean{TypeKind::Bool}, boolean2{TypeKind::Bool, 2};
  const uint32_t extent = operation(104, integer2, image);
  const uint32_t below =
      operation(154, boolean, operation(177, boolean2, coord, constant(integer2, 0)));
  const uint32_t above = operation(154, boolean, operation(175, boolean2, coord, extent));
  const uint32_t condition = operation(168, boolean, operation(166, boolean, below, above));
  const uint32_t storeBlock = id(), merge = id();
  functions_.instruction(247, merge, 0);
  functions_.instruction(250, condition, storeBlock, merge);
  label(storeBlock);
  functions_.instruction(99, image, coord, value);
  branch(merge);
  label(merge);
}

constexpr void Emitter::emitBlock(ArenaId index) {
  uint16_t visited = 0;
  for (ArenaId next = index; next != kInvalidArenaId && !terminated_ && result_.isSuccess();) {
    if (next >= module_.statementCount || ++visited > ModuleLimits::kMaxStatements) {
      fail(SpirvEmitError::InvalidNode);
      return;
    }
    const Statement& statement = module_.statements[next];
    emitStatement(statement);
    next = statement.next;
  }
}

constexpr void Emitter::emitDeclaration(const Statement& node) {
  if (node.symbolId >= module_.symbolCount) {
    fail(SpirvEmitError::InvalidNode);
    return;
  }
  const Symbol& symbol = module_.symbols[node.symbolId];
  const uint32_t value = node.expression == kInvalidArenaId ? constant(symbol.type, 0)
                                                            : emitExpression(node.expression);
  if (localStorage(symbol))
    functions_.instruction(62, symbolValues_[node.symbolId], value);
  else
    symbolValues_[node.symbolId] = value;
}

constexpr void Emitter::emitAssignment(const Statement& node) {
  uint32_t storage = 0;
  const uint32_t pointer = lvalue(node.expression, storage);
  const uint32_t value = emitExpression(node.secondExpression);
  if (storage != 7) {
    fail(SpirvEmitError::InvalidNode, node.span);
    return;
  }
  functions_.instruction(62, pointer, value);
}

constexpr void Emitter::emitIf(const Statement& node) {
  const uint32_t condition = emitExpression(node.expression);
  const uint32_t yes = id(), no = id(), merge = id();
  functions_.instruction(247, merge, 0);
  functions_.instruction(250, condition, yes, no);
  label(yes);
  emitBlock(node.firstBody);
  const bool yesTerminates = terminated_;
  branch(merge);
  label(no);
  emitBlock(node.firstElseBody);
  const bool noTerminates = terminated_;
  branch(merge);
  label(merge);
  if (yesTerminates && noTerminates) {
    functions_.instruction(255);
    terminated_ = true;
  }
}

constexpr void Emitter::emitSwitchSelectors(
    const std::array<ArenaId, ModuleLimits::kMaxStatements>& clauses,
    const std::array<uint32_t, ModuleLimits::kMaxStatements>& labels, uint16_t count,
    SourceSpan span) {
  for (uint16_t i = 0; i < count; ++i) {
    const ArenaId value = module_.statements[clauses[i]].expression;
    if (value == kInvalidArenaId) continue;
    if (value >= module_.expressionCount ||
        module_.expressions[value].kind != ExpressionKind::Literal) {
      fail(SpirvEmitError::InvalidNode, span);
      return;
    }
    functions_.word(module_.expressions[value].payload);
    functions_.word(labels[i]);
  }
}

constexpr void Emitter::emitSwitch(const Statement& node) {
  const uint32_t selector = emitExpression(node.expression);
  const uint32_t merge = id();
  uint32_t defaultLabel = merge;
  std::array<ArenaId, ModuleLimits::kMaxStatements> clauses{};
  std::array<uint32_t, ModuleLimits::kMaxStatements> labels{};
  uint16_t count = 0, selectorCount = 0;
  for (ArenaId current = node.firstBody; current != kInvalidArenaId;
       current = module_.statements[current].next) {
    if (current >= module_.statementCount || count == clauses.size() ||
        module_.statements[current].kind != StatementKind::Case) {
      fail(SpirvEmitError::InvalidNode, node.span);
      return;
    }
    clauses[count] = current;
    labels[count] = id();
    if (module_.statements[current].expression == kInvalidArenaId)
      defaultLabel = labels[count];
    else
      ++selectorCount;
    ++count;
  }
  functions_.instruction(247, merge, 0);
  functions_.word((uint32_t(3 + 2 * selectorCount) << 16) | 251u);
  functions_.word(selector);
  functions_.word(defaultLabel);
  emitSwitchSelectors(clauses, labels, count, node.span);
  const uint32_t outerBreak = breakTarget_;
  breakTarget_ = merge;
  for (uint16_t i = 0; i < count; ++i) {
    label(labels[i]);
    emitBlock(module_.statements[clauses[i]].firstBody);
    branch(merge);
  }
  breakTarget_ = outerBreak;
  label(merge);
  if (node.alwaysTerminates) {
    functions_.instruction(255);
    terminated_ = true;
  }
}

constexpr void Emitter::emitFor(const Statement& node) {
  if (node.init >= module_.statementCount || node.continuing >= module_.statementCount) {
    fail(SpirvEmitError::InvalidNode, node.span);
    return;
  }
  emitStatement(module_.statements[node.init]);
  const uint32_t header = id(), conditionBlock = id(), body = id(), continuing = id(), merge = id();
  branch(header);
  label(header);
  functions_.instruction(246, merge, continuing, 0);
  branch(conditionBlock);
  label(conditionBlock);
  const uint32_t condition = emitExpression(node.expression);
  functions_.instruction(250, condition, body, merge);
  label(body);
  const uint32_t outerBreak = breakTarget_, outerContinue = continueTarget_;
  breakTarget_ = merge;
  continueTarget_ = continuing;
  emitBlock(node.firstBody);
  breakTarget_ = outerBreak;
  continueTarget_ = outerContinue;
  branch(continuing);
  label(continuing);
  emitStatement(module_.statements[node.continuing]);
  branch(header);
  label(merge);
}

constexpr void Emitter::emitWhile(const Statement& node) {
  const uint32_t header = id(), conditionBlock = id(), body = id(), continuing = id(), merge = id();
  branch(header);
  label(header);
  functions_.instruction(246, merge, continuing, 0);
  branch(conditionBlock);
  label(conditionBlock);
  const uint32_t condition = emitExpression(node.expression);
  functions_.instruction(250, condition, body, merge);
  label(body);
  const uint32_t outerBreak = breakTarget_, outerContinue = continueTarget_;
  breakTarget_ = merge;
  continueTarget_ = continuing;
  emitBlock(node.firstBody);
  breakTarget_ = outerBreak;
  continueTarget_ = outerContinue;
  branch(continuing);
  label(continuing);
  branch(header);
  label(merge);
}

constexpr void Emitter::emitLoop(const Statement& node) {
  const uint32_t header = id(), body = id(), continuing = id(), merge = id();
  branch(header);
  label(header);
  functions_.instruction(246, merge, continuing, 0);
  branch(body);
  label(body);
  const uint32_t outerBreak = breakTarget_, outerContinue = continueTarget_;
  breakTarget_ = merge;
  continueTarget_ = continuing;
  emitBlock(node.firstBody);
  breakTarget_ = outerBreak;
  continueTarget_ = outerContinue;
  branch(continuing);
  label(continuing);
  branch(header);
  label(merge);
}

constexpr void Emitter::emitLoopExit(const Statement& node) {
  const uint32_t target = node.kind == StatementKind::Break ? breakTarget_ : continueTarget_;
  if (target == 0)
    fail(SpirvEmitError::InvalidNode, node.span);
  else
    branch(target);
}

constexpr void Emitter::emitControlStatement(const Statement& node) {
  switch (node.kind) {
    default: fail(SpirvEmitError::InvalidNode, node.span); break;
    case StatementKind::Break:
    case StatementKind::Continue: emitLoopExit(node); break;
    case StatementKind::Discard:
      functions_.instruction(252);
      terminated_ = true;
      break;
    case StatementKind::Return:
      emitReturn(node.expression);
      terminated_ = true;
      break;
  }
}

constexpr void Emitter::emitStatement(const Statement& node) {
  switch (node.kind) {
    case StatementKind::Declaration: emitDeclaration(node); break;
    case StatementKind::Assign: emitAssignment(node); break;
    case StatementKind::If: emitIf(node); break;
    case StatementKind::For: emitFor(node); break;
    case StatementKind::Call: emitExpression(node.expression); break;
    case StatementKind::While: emitWhile(node); break;
    case StatementKind::Loop: emitLoop(node); break;
    case StatementKind::Switch: emitSwitch(node); break;
    default: emitControlStatement(node); break;
    case StatementKind::TextureStore: {
      const uint32_t image = emitExpression(node.expression);
      const uint32_t coordinate = emitExpression(node.secondExpression);
      const uint32_t value = emitExpression(node.thirdExpression);
      robustStore(image, coordinate, value);
      break;
    }
  }
}

}  // namespace spirv_detail

/// Emits SPIR-V 1.3 for Vulkan 1.1 from a successfully validated module.
/// @param module Immutable validated module. @param output Caller-owned word sink.
constexpr SpirvEmitResult EmitSpirv(const Module& module, SpirvSink& output) {
  output.size = 0;
  output.error = SpirvEmitError::None;
  spirv_detail::Emitter emitter(module);
  const auto result = emitter.emit(output);
  if (!result.isSuccess()) {
    output.size = 0;
    output.error = result.error;
  }
  return result;
}

}  // namespace donner::gpu::shader::wgsl
