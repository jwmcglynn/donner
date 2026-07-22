#include "donner/gpu/shader/SpirvEmitter.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "donner/gpu/shader/IrLayout.h"

namespace donner::gpu::shader {

namespace {

// Binary constants from the public SPIR-V 1.3 specification (and the GLSL.std.450 extended
// instruction set specification). Only the subset this emitter produces is listed.

constexpr uint32_t kSpirvMagic = 0x07230203;
constexpr uint32_t kSpirvVersion13 = 0x00010300;

// Opcodes.
constexpr uint32_t kOpExtInstImport = 11;
constexpr uint32_t kOpExtInst = 12;
constexpr uint32_t kOpMemoryModel = 14;
constexpr uint32_t kOpEntryPoint = 15;
constexpr uint32_t kOpExecutionMode = 16;
constexpr uint32_t kOpCapability = 17;
constexpr uint32_t kOpTypeVoid = 19;
constexpr uint32_t kOpTypeBool = 20;
constexpr uint32_t kOpTypeInt = 21;
constexpr uint32_t kOpTypeFloat = 22;
constexpr uint32_t kOpTypeVector = 23;
constexpr uint32_t kOpTypeMatrix = 24;
constexpr uint32_t kOpTypeImage = 25;
constexpr uint32_t kOpTypeSampler = 26;
constexpr uint32_t kOpTypeSampledImage = 27;
constexpr uint32_t kOpTypeArray = 28;
constexpr uint32_t kOpTypeRuntimeArray = 29;
constexpr uint32_t kOpTypeStruct = 30;
constexpr uint32_t kOpTypePointer = 32;
constexpr uint32_t kOpTypeFunction = 33;
constexpr uint32_t kOpConstantTrue = 41;
constexpr uint32_t kOpConstantFalse = 42;
constexpr uint32_t kOpConstant = 43;
constexpr uint32_t kOpConstantComposite = 44;
constexpr uint32_t kOpFunction = 54;
constexpr uint32_t kOpFunctionParameter = 55;
constexpr uint32_t kOpFunctionEnd = 56;
constexpr uint32_t kOpFunctionCall = 57;
constexpr uint32_t kOpVariable = 59;
constexpr uint32_t kOpLoad = 61;
constexpr uint32_t kOpStore = 62;
constexpr uint32_t kOpAccessChain = 65;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kOpMemberDecorate = 72;
constexpr uint32_t kOpVectorExtractDynamic = 77;
constexpr uint32_t kOpVectorShuffle = 79;
constexpr uint32_t kOpCompositeConstruct = 80;
constexpr uint32_t kOpCompositeExtract = 81;
constexpr uint32_t kOpSampledImage = 86;
constexpr uint32_t kOpImageSampleImplicitLod = 87;
constexpr uint32_t kOpImageFetch = 95;
constexpr uint32_t kOpImageQuerySizeLod = 103;
constexpr uint32_t kOpConvertFToU = 109;
constexpr uint32_t kOpConvertFToS = 110;
constexpr uint32_t kOpConvertSToF = 111;
constexpr uint32_t kOpConvertUToF = 112;
constexpr uint32_t kOpBitcast = 124;
constexpr uint32_t kOpSNegate = 126;
constexpr uint32_t kOpFNegate = 127;
constexpr uint32_t kOpIAdd = 128;
constexpr uint32_t kOpFAdd = 129;
constexpr uint32_t kOpISub = 130;
constexpr uint32_t kOpFSub = 131;
constexpr uint32_t kOpIMul = 132;
constexpr uint32_t kOpFMul = 133;
constexpr uint32_t kOpUDiv = 134;
constexpr uint32_t kOpSDiv = 135;
constexpr uint32_t kOpFDiv = 136;
constexpr uint32_t kOpVectorTimesScalar = 142;
constexpr uint32_t kOpMatrixTimesVector = 145;
constexpr uint32_t kOpMatrixTimesMatrix = 146;
constexpr uint32_t kOpLogicalEqual = 164;
constexpr uint32_t kOpLogicalNotEqual = 165;
constexpr uint32_t kOpLogicalOr = 166;
constexpr uint32_t kOpLogicalAnd = 167;
constexpr uint32_t kOpLogicalNot = 168;
constexpr uint32_t kOpSelect = 169;
constexpr uint32_t kOpIEqual = 170;
constexpr uint32_t kOpINotEqual = 171;
constexpr uint32_t kOpUGreaterThan = 172;
constexpr uint32_t kOpSGreaterThan = 173;
constexpr uint32_t kOpUGreaterThanEqual = 174;
constexpr uint32_t kOpSGreaterThanEqual = 175;
constexpr uint32_t kOpULessThan = 176;
constexpr uint32_t kOpSLessThan = 177;
constexpr uint32_t kOpULessThanEqual = 178;
constexpr uint32_t kOpSLessThanEqual = 179;
constexpr uint32_t kOpFOrdEqual = 180;
constexpr uint32_t kOpFOrdNotEqual = 182;
constexpr uint32_t kOpFOrdLessThan = 184;
constexpr uint32_t kOpFOrdGreaterThan = 186;
constexpr uint32_t kOpFOrdLessThanEqual = 188;
constexpr uint32_t kOpFOrdGreaterThanEqual = 190;
constexpr uint32_t kOpFwidth = 209;
constexpr uint32_t kOpLoopMerge = 246;
constexpr uint32_t kOpSelectionMerge = 247;
constexpr uint32_t kOpLabel = 248;
constexpr uint32_t kOpBranch = 249;
constexpr uint32_t kOpBranchConditional = 250;
constexpr uint32_t kOpKill = 252;
constexpr uint32_t kOpReturn = 253;
constexpr uint32_t kOpReturnValue = 254;
constexpr uint32_t kOpUnreachable = 255;

// GLSL.std.450 extended instruction numbers.
constexpr uint32_t kGlslRoundEven = 2;
constexpr uint32_t kGlslFAbs = 4;
constexpr uint32_t kGlslSAbs = 5;
constexpr uint32_t kGlslFract = 10;
constexpr uint32_t kGlslSqrt = 31;
constexpr uint32_t kGlslFMin = 37;
constexpr uint32_t kGlslUMin = 38;
constexpr uint32_t kGlslSMin = 39;
constexpr uint32_t kGlslFMax = 40;
constexpr uint32_t kGlslUMax = 41;
constexpr uint32_t kGlslSMax = 42;
constexpr uint32_t kGlslFClamp = 43;
constexpr uint32_t kGlslUClamp = 44;
constexpr uint32_t kGlslSClamp = 45;
constexpr uint32_t kGlslLength = 66;

// Storage classes.
constexpr uint32_t kStorageClassUniformConstant = 0;
constexpr uint32_t kStorageClassInput = 1;
constexpr uint32_t kStorageClassUniform = 2;
constexpr uint32_t kStorageClassOutput = 3;
constexpr uint32_t kStorageClassFunction = 7;
constexpr uint32_t kStorageClassStorageBuffer = 12;

// Decorations.
constexpr uint32_t kDecorationBlock = 2;
constexpr uint32_t kDecorationColMajor = 5;
constexpr uint32_t kDecorationArrayStride = 6;
constexpr uint32_t kDecorationMatrixStride = 7;
constexpr uint32_t kDecorationBuiltIn = 11;
constexpr uint32_t kDecorationFlat = 14;
constexpr uint32_t kDecorationNonWritable = 24;
constexpr uint32_t kDecorationLocation = 30;
constexpr uint32_t kDecorationBinding = 33;
constexpr uint32_t kDecorationDescriptorSet = 34;
constexpr uint32_t kDecorationOffset = 35;

// BuiltIn values.
constexpr uint32_t kBuiltInPosition = 0;
constexpr uint32_t kBuiltInFragCoord = 15;
constexpr uint32_t kBuiltInInstanceIndex = 43;

// Execution models, modes, and module-level enums.
constexpr uint32_t kExecutionModelVertex = 0;
constexpr uint32_t kExecutionModelFragment = 4;
constexpr uint32_t kExecutionModeOriginUpperLeft = 7;
constexpr uint32_t kAddressingModelLogical = 0;
constexpr uint32_t kMemoryModelGlsl450 = 1;
constexpr uint32_t kCapabilityShader = 1;
constexpr uint32_t kCapabilityImageQuery = 50;
constexpr uint32_t kImageOperandsLodMask = 0x2;
constexpr uint32_t kDim2D = 1;
constexpr uint32_t kImageFormatUnknown = 0;
constexpr uint32_t kFunctionControlNone = 0;
constexpr uint32_t kSelectionControlNone = 0;
constexpr uint32_t kLoopControlNone = 0;

/// Appends one instruction with the given literal operand words to \p section.
void Instr(std::vector<uint32_t>& section, uint32_t opcode, std::initializer_list<uint32_t> ops) {
  section.push_back(((static_cast<uint32_t>(ops.size()) + 1) << 16) | opcode);
  section.insert(section.end(), ops);
}

/// Appends one instruction with a dynamic operand list to \p section.
void InstrV(std::vector<uint32_t>& section, uint32_t opcode, const std::vector<uint32_t>& ops) {
  section.push_back(((static_cast<uint32_t>(ops.size()) + 1) << 16) | opcode);
  section.insert(section.end(), ops.begin(), ops.end());
}

/// Encodes a UTF-8/ASCII string as SPIR-V literal words: nul-terminated, little-endian packed,
/// padded with zero bytes to a word boundary.
std::vector<uint32_t> EncodeString(std::string_view text) {
  std::vector<uint32_t> words((text.size() / 4) + 1, 0);
  for (size_t i = 0; i < text.size(); ++i) {
    words[i / 4] |= static_cast<uint32_t>(static_cast<unsigned char>(text[i])) << (8 * (i % 4));
  }
  return words;
}

/// Zero-based component index of a swizzle character.
uint32_t SwizzleComponentIndex(char component) {
  switch (component) {
    case 'x': return 0;
    case 'y': return 1;
    case 'z': return 2;
    case 'w': return 3;
    default: return 0;
  }
}

/// True if \p type is or contains a matrix (the struct member holding it needs ColMajor and
/// MatrixStride decorations in laid-out buffer types).
bool ContainsMatrix(const IrType& type) {
  switch (type.kind()) {
    case IrType::Kind::Matrix4x4f: return true;
    case IrType::Kind::SizedArray:
    case IrType::Kind::RuntimeArray: return ContainsMatrix(type.elementType());
    default: return false;
  }
}

/// Deterministic SPIR-V emitter: single ID counter, fixed traversal order, ordered dedup maps
/// keyed by structural strings (see the SpirvEmitter.h contract).
class Emitter {
public:
  explicit Emitter(const IrModule& module) : module_(module) {}

  /// Runs emission. @return The SPIR-V word stream or the first error.
  ShaderResult<std::vector<uint32_t>> emit();

private:
  /// A resolved module-scope resource binding.
  struct BindingInfo {
    uint32_t varId = 0;                             //!< Module-scope OpVariable id.
    BindingKind kind = BindingKind::UniformBuffer;  //!< Binding kind.
    IrType type = IrType::F32();                    //!< Bound root type.
    uint32_t storageClass = 0;                      //!< SPIR-V storage class of the variable.
    bool wrapped = false;  //!< True when the root is a runtime array wrapped in a Block struct
                           //!< (accesses prepend a constant 0 member index).
  };

  /// A name visible in the current function scope.
  struct Local {
    RefKind kind = RefKind::Let;  //!< What the name refers to.
    uint32_t id = 0;              //!< Value id (param/let) or Function-storage pointer id (var).
    IrType type = IrType::F32();  //!< Declared type.
  };

  /// A callable module function.
  struct FunctionInfo {
    uint32_t id = 0;            //!< OpFunction id.
    uint32_t returnTypeId = 0;  //!< Return type id (void id for void functions).
  };

  /// A pointer access chain rooted at a Function-storage var or a buffer binding.
  struct Chain {
    uint32_t base = 0;                  //!< Root variable id.
    uint32_t storageClass = 0;          //!< Storage class of the root variable.
    std::optional<AddressSpace> space;  //!< Layout address space for buffer roots.
    std::vector<uint32_t> indices;      //!< Access chain index ids, outermost first.
  };

  // ----- Error latching -----

  /// Latches the first error; subsequent ids are dummies discarded with the output.
  void latch(std::string message) {
    if (!error_) {
      error_ = ShaderError{std::move(message), "spirv"};
    }
  }
  /// Latches an error propagated from the layout engine.
  void latchError(ShaderError&& error) {
    if (!error_) {
      error_ = std::move(error);
    }
  }

  /// Allocates the next result id from the single module-wide counter.
  uint32_t newId() { return nextId_++; }

  // ----- Types and constants (created on first use, deduplicated by structural key) -----

  uint32_t cached(const std::string& key) const {
    const auto it = typeIds_.find(key);
    return it != typeIds_.end() ? it->second : 0;
  }

  uint32_t typeVoid();
  uint32_t typeScalar(ScalarKind kind);
  uint32_t typeVector(ScalarKind kind, uint32_t size);
  uint32_t typeBoolVector(uint32_t size);
  uint32_t typeMatrix4x4f();
  uint32_t typeImage2dF32();
  uint32_t typeSampledImage();
  uint32_t typeSampler();
  uint32_t typePointer(uint32_t storageClass, uint32_t pointeeId);
  uint32_t typeFunction(uint32_t returnTypeId, const std::vector<uint32_t>& paramTypeIds);

  /// Undecorated type used for function-local values.
  uint32_t plainTypeId(const IrType& type);
  /// Layout-decorated type used inside buffer blocks (Offset / ArrayStride / ColMajor /
  /// MatrixStride from the IrLayout engine). Scalars, vectors, and matrices share the plain ids;
  /// structs and arrays get distinct decorated ids.
  uint32_t laidTypeId(const IrType& type, AddressSpace space);
  /// Block-decorated buffer root struct (distinct from the non-Block laid-out type).
  uint32_t blockStructId(const IrType& structType, AddressSpace space);
  /// Synthesized Block struct wrapping a runtime-array storage buffer root at member 0.
  uint32_t runtimeArrayBlockId(uint32_t runtimeArrayTypeId);

  uint32_t constBool(bool value);
  uint32_t constU32(uint32_t value);
  uint32_t constI32(int32_t value);
  uint32_t constF32(float value);
  /// OpConstantComposite splat of \p scalarId across a vector type.
  uint32_t constSplat(uint32_t vectorTypeId, uint32_t scalarId, uint32_t size);

  // ----- Module-scope emission -----

  void emitBindings();
  void emitModuleConstants();
  void emitFunction(const IrFunction& function);

  // ----- Function-body emission -----

  void pushScope() { scopes_.emplace_back(); }
  void popScope() { scopes_.pop_back(); }
  void bindLocal(const RcString& name, Local local) {
    scopes_.back().insert_or_assign(name.str(), std::move(local));
  }
  const Local* resolveLocal(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  /// Declares Function-storage OpVariables for every var (and for-loop init var) in \p block,
  /// recursively, into the entry block per the SPIR-V specification.
  void hoistFunctionVariables(const IrBlock& block);

  void startBlock(uint32_t labelId) {
    Instr(functions_, kOpLabel, {labelId});
    blockTerminated_ = false;
  }

  void emitBlock(const IrBlock& block);
  void emitStatement(const IrStmt& statement);
  void emitAssign(const IrExpr& lhs, const IrExpr& rhs);

  /// True if \p expr is a member/index/single-component-swizzle chain rooted at a `var` or a
  /// buffer binding, i.e. it can be lowered to an access chain.
  bool isPointerBacked(const IrExpr& expr) const;
  /// Builds the access chain for a pointer-backed expression, evaluating dynamic indices in
  /// operand order. @pre isPointerBacked(expr).
  Chain emitChain(const IrExpr& expr);
  /// Loads the value a chain points to. Buffer-side struct and sized-array values are rebuilt
  /// member-by-member into their plain types (SPIR-V 1.3 has no OpCopyLogical); runtime arrays
  /// fail closed.
  uint32_t loadThroughChain(const Chain& chain, const IrType& type);
  /// Recursively rebuilds a plain composite value from a laid-out buffer pointer.
  uint32_t rebuildFromBuffer(uint32_t pointerId, const IrType& type, uint32_t storageClass,
                             AddressSpace space);

  uint32_t emitValue(const IrExpr& expr);
  uint32_t emitLiteral(const IrExpr::Node& node);
  uint32_t emitRefValue(const IrExpr::Node& node);
  uint32_t emitBinary(const IrExpr::Node& node);
  uint32_t emitConvert(const IrExpr::Node& node);
  uint32_t emitBuiltinCall(const IrExpr::Node& node);
  /// OpExtInst into the GLSL.std.450 import.
  uint32_t emitExtInst(uint32_t typeId, uint32_t instruction, std::vector<uint32_t> operands);
  /// Splats a scalar id across a vector type with OpCompositeConstruct.
  uint32_t emitSplat(const IrType& vectorType, uint32_t scalarId);

  const IrModule& module_;
  std::optional<ShaderError> error_;
  uint32_t nextId_ = 1;
  uint32_t glslImportId_ = 0;
  bool usesImageQuery_ = false;

  // Sections assembled in the standard SPIR-V order at the end.
  std::vector<uint32_t> entryPoints_;
  std::vector<uint32_t> executionModes_;
  std::vector<uint32_t> decorations_;
  std::vector<uint32_t> globals_;  //!< Types, constants, and module-scope variables.
  std::vector<uint32_t> functions_;

  std::map<std::string, uint32_t> typeIds_;
  std::map<std::string, uint32_t> constantIds_;
  std::map<std::string, uint32_t> moduleConstants_;
  std::map<std::string, BindingInfo> bindings_;
  std::map<std::string, FunctionInfo> functionIds_;

  // Per-function state.
  StageKind currentStage_ = StageKind::None;
  std::string currentFunctionName_;
  std::vector<std::map<std::string, Local>> scopes_;
  std::unordered_map<const IrStmt::Data*, uint32_t> hoistedVars_;
  std::vector<std::pair<uint32_t, uint32_t>> loopStack_;  //!< (merge, continue) label ids.
  std::vector<uint32_t> currentOutputVars_;
  bool blockTerminated_ = false;
};

// ----- Types and constants -----

uint32_t Emitter::typeVoid() {
  if (const uint32_t id = cached("void")) return id;
  const uint32_t id = newId();
  Instr(globals_, kOpTypeVoid, {id});
  typeIds_["void"] = id;
  return id;
}

uint32_t Emitter::typeScalar(ScalarKind kind) {
  std::string key;
  switch (kind) {
    case ScalarKind::Bool: key = "bool"; break;
    case ScalarKind::I32: key = "i32"; break;
    case ScalarKind::U32: key = "u32"; break;
    case ScalarKind::F32: key = "f32"; break;
  }
  if (const uint32_t id = cached(key)) return id;
  const uint32_t id = newId();
  switch (kind) {
    case ScalarKind::Bool: Instr(globals_, kOpTypeBool, {id}); break;
    case ScalarKind::I32: Instr(globals_, kOpTypeInt, {id, 32, 1}); break;
    case ScalarKind::U32: Instr(globals_, kOpTypeInt, {id, 32, 0}); break;
    case ScalarKind::F32: Instr(globals_, kOpTypeFloat, {id, 32}); break;
  }
  typeIds_[key] = id;
  return id;
}

uint32_t Emitter::typeVector(ScalarKind kind, uint32_t size) {
  const uint32_t componentId = typeScalar(kind);
  const std::string key = std::format("vec|{}|{}", componentId, size);
  if (const uint32_t id = cached(key)) return id;
  const uint32_t id = newId();
  Instr(globals_, kOpTypeVector, {id, componentId, size});
  typeIds_[key] = id;
  return id;
}

uint32_t Emitter::typeBoolVector(uint32_t size) {
  return typeVector(ScalarKind::Bool, size);
}

uint32_t Emitter::typeMatrix4x4f() {
  const uint32_t columnId = typeVector(ScalarKind::F32, 4);
  const std::string key = "mat4x4f";
  if (const uint32_t id = cached(key)) return id;
  const uint32_t id = newId();
  Instr(globals_, kOpTypeMatrix, {id, columnId, 4});
  typeIds_[key] = id;
  return id;
}

uint32_t Emitter::typeImage2dF32() {
  const uint32_t sampledType = typeScalar(ScalarKind::F32);
  const std::string key = "image2df32";
  if (const uint32_t id = cached(key)) return id;
  const uint32_t id = newId();
  // 2D, non-depth, non-arrayed, single-sampled, sampled (usable with a sampler), format unknown.
  Instr(globals_, kOpTypeImage, {id, sampledType, kDim2D, 0, 0, 0, 1, kImageFormatUnknown});
  typeIds_[key] = id;
  return id;
}

uint32_t Emitter::typeSampledImage() {
  const uint32_t imageId = typeImage2dF32();
  const std::string key = "sampledimage";
  if (const uint32_t id = cached(key)) return id;
  const uint32_t id = newId();
  Instr(globals_, kOpTypeSampledImage, {id, imageId});
  typeIds_[key] = id;
  return id;
}

uint32_t Emitter::typeSampler() {
  const std::string key = "sampler";
  if (const uint32_t id = cached(key)) return id;
  const uint32_t id = newId();
  Instr(globals_, kOpTypeSampler, {id});
  typeIds_[key] = id;
  return id;
}

uint32_t Emitter::typePointer(uint32_t storageClass, uint32_t pointeeId) {
  const std::string key = std::format("ptr|{}|{}", storageClass, pointeeId);
  if (const uint32_t id = cached(key)) return id;
  const uint32_t id = newId();
  Instr(globals_, kOpTypePointer, {id, storageClass, pointeeId});
  typeIds_[key] = id;
  return id;
}

uint32_t Emitter::typeFunction(uint32_t returnTypeId, const std::vector<uint32_t>& paramTypeIds) {
  std::string key = std::format("fn|{}", returnTypeId);
  for (const uint32_t paramTypeId : paramTypeIds) {
    key += std::format("|{}", paramTypeId);
  }
  if (const uint32_t id = cached(key)) return id;
  const uint32_t id = newId();
  std::vector<uint32_t> operands = {id, returnTypeId};
  operands.insert(operands.end(), paramTypeIds.begin(), paramTypeIds.end());
  InstrV(globals_, kOpTypeFunction, operands);
  typeIds_[key] = id;
  return id;
}

uint32_t Emitter::plainTypeId(const IrType& type) {
  switch (type.kind()) {
    case IrType::Kind::Scalar: return typeScalar(type.scalarKind());
    case IrType::Kind::Vector: return typeVector(type.scalarKind(), type.vectorSize());
    case IrType::Kind::Matrix4x4f: return typeMatrix4x4f();
    case IrType::Kind::Texture2dF32: return typeImage2dF32();
    case IrType::Kind::Sampler: return typeSampler();
    case IrType::Kind::SizedArray: {
      const uint32_t elementId = plainTypeId(type.elementType());
      const std::string key = std::format("arr|{}|{}", elementId, type.arrayCount());
      if (const uint32_t id = cached(key)) return id;
      const uint32_t lengthId = constU32(type.arrayCount());
      const uint32_t id = newId();
      Instr(globals_, kOpTypeArray, {id, elementId, lengthId});
      typeIds_[key] = id;
      return id;
    }
    case IrType::Kind::Struct: {
      std::string key = std::format("struct|{}", type.structName().str());
      std::vector<uint32_t> memberIds;
      for (const IrType::Member& member : type.structMembers()) {
        memberIds.push_back(plainTypeId(member.type));
        key += std::format("|{}", memberIds.back());
      }
      if (const uint32_t id = cached(key)) return id;
      const uint32_t id = newId();
      std::vector<uint32_t> operands = {id};
      operands.insert(operands.end(), memberIds.begin(), memberIds.end());
      InstrV(globals_, kOpTypeStruct, operands);
      typeIds_[key] = id;
      return id;
    }
    case IrType::Kind::RuntimeArray:
      latch(
          "a runtime-sized array has no value type; it can only be accessed through a storage "
          "buffer binding");
      return 0;
  }
  return 0;
}

uint32_t Emitter::laidTypeId(const IrType& type, AddressSpace space) {
  switch (type.kind()) {
    case IrType::Kind::Scalar:
    case IrType::Kind::Vector:
    case IrType::Kind::Matrix4x4f:
      // Layout decorations for these live on the enclosing struct member; the types are shared
      // with plain use.
      return plainTypeId(type);
    case IrType::Kind::SizedArray: {
      ShaderResult<uint32_t> stride = ComputeArrayStride(type, space);
      if (stride.hasError()) {
        latchError(std::move(stride).error());
        return 0;
      }
      const uint32_t elementId = laidTypeId(type.elementType(), space);
      const std::string key =
          std::format("arrL|{}|{}|{}", elementId, type.arrayCount(), stride.result());
      if (const uint32_t id = cached(key)) return id;
      const uint32_t lengthId = constU32(type.arrayCount());
      const uint32_t id = newId();
      Instr(globals_, kOpTypeArray, {id, elementId, lengthId});
      Instr(decorations_, kOpDecorate, {id, kDecorationArrayStride, stride.result()});
      typeIds_[key] = id;
      return id;
    }
    case IrType::Kind::RuntimeArray: {
      ShaderResult<uint32_t> stride = ComputeArrayStride(type, space);
      if (stride.hasError()) {
        latchError(std::move(stride).error());
        return 0;
      }
      const uint32_t elementId = laidTypeId(type.elementType(), space);
      const std::string key = std::format("rta|{}|{}", elementId, stride.result());
      if (const uint32_t id = cached(key)) return id;
      const uint32_t id = newId();
      Instr(globals_, kOpTypeRuntimeArray, {id, elementId});
      Instr(decorations_, kOpDecorate, {id, kDecorationArrayStride, stride.result()});
      typeIds_[key] = id;
      return id;
    }
    case IrType::Kind::Struct: {
      ShaderResult<StructLayout> layout = ComputeStructLayout(type, space);
      if (layout.hasError()) {
        latchError(std::move(layout).error());
        return 0;
      }
      std::string key = std::format("structL|{}", type.structName().str());
      std::vector<uint32_t> memberIds;
      const std::span<const IrType::Member> members = type.structMembers();
      for (size_t i = 0; i < members.size(); ++i) {
        memberIds.push_back(laidTypeId(members[i].type, space));
        key += std::format("|{}@{}", memberIds.back(), layout.result().members[i].offsetBytes);
      }
      if (const uint32_t id = cached(key)) return id;
      const uint32_t id = newId();
      std::vector<uint32_t> operands = {id};
      operands.insert(operands.end(), memberIds.begin(), memberIds.end());
      InstrV(globals_, kOpTypeStruct, operands);
      for (size_t i = 0; i < members.size(); ++i) {
        const uint32_t memberIndex = static_cast<uint32_t>(i);
        Instr(decorations_, kOpMemberDecorate,
              {id, memberIndex, kDecorationOffset, layout.result().members[i].offsetBytes});
        if (ContainsMatrix(members[i].type)) {
          Instr(decorations_, kOpMemberDecorate, {id, memberIndex, kDecorationColMajor});
          Instr(decorations_, kOpMemberDecorate, {id, memberIndex, kDecorationMatrixStride, 16});
        }
      }
      typeIds_[key] = id;
      return id;
    }
    case IrType::Kind::Texture2dF32:
    case IrType::Kind::Sampler:
      latch(std::format("type {} has no host-shareable layout", type.toString()));
      return 0;
  }
  return 0;
}

uint32_t Emitter::blockStructId(const IrType& structType, AddressSpace space) {
  ShaderResult<StructLayout> layout = ComputeStructLayout(structType, space);
  if (layout.hasError()) {
    latchError(std::move(layout).error());
    return 0;
  }
  std::string key = std::format("blk|{}", structType.structName().str());
  std::vector<uint32_t> memberIds;
  const std::span<const IrType::Member> members = structType.structMembers();
  for (size_t i = 0; i < members.size(); ++i) {
    memberIds.push_back(laidTypeId(members[i].type, space));
    key += std::format("|{}@{}", memberIds.back(), layout.result().members[i].offsetBytes);
  }
  if (const uint32_t id = cached(key)) return id;
  const uint32_t id = newId();
  std::vector<uint32_t> operands = {id};
  operands.insert(operands.end(), memberIds.begin(), memberIds.end());
  InstrV(globals_, kOpTypeStruct, operands);
  Instr(decorations_, kOpDecorate, {id, kDecorationBlock});
  for (size_t i = 0; i < members.size(); ++i) {
    const uint32_t memberIndex = static_cast<uint32_t>(i);
    Instr(decorations_, kOpMemberDecorate,
          {id, memberIndex, kDecorationOffset, layout.result().members[i].offsetBytes});
    if (ContainsMatrix(members[i].type)) {
      Instr(decorations_, kOpMemberDecorate, {id, memberIndex, kDecorationColMajor});
      Instr(decorations_, kOpMemberDecorate, {id, memberIndex, kDecorationMatrixStride, 16});
    }
  }
  typeIds_[key] = id;
  return id;
}

uint32_t Emitter::runtimeArrayBlockId(uint32_t runtimeArrayTypeId) {
  const std::string key = std::format("wrap|{}", runtimeArrayTypeId);
  if (const uint32_t id = cached(key)) return id;
  const uint32_t id = newId();
  Instr(globals_, kOpTypeStruct, {id, runtimeArrayTypeId});
  Instr(decorations_, kOpDecorate, {id, kDecorationBlock});
  Instr(decorations_, kOpMemberDecorate, {id, 0, kDecorationOffset, 0});
  typeIds_[key] = id;
  return id;
}

uint32_t Emitter::constBool(bool value) {
  const std::string key = value ? "c|true" : "c|false";
  const auto it = constantIds_.find(key);
  if (it != constantIds_.end()) return it->second;
  const uint32_t typeId = typeScalar(ScalarKind::Bool);
  const uint32_t id = newId();
  Instr(globals_, value ? kOpConstantTrue : kOpConstantFalse, {typeId, id});
  constantIds_[key] = id;
  return id;
}

uint32_t Emitter::constU32(uint32_t value) {
  const std::string key = std::format("c|u32|{}", value);
  const auto it = constantIds_.find(key);
  if (it != constantIds_.end()) return it->second;
  const uint32_t typeId = typeScalar(ScalarKind::U32);
  const uint32_t id = newId();
  Instr(globals_, kOpConstant, {typeId, id, value});
  constantIds_[key] = id;
  return id;
}

uint32_t Emitter::constI32(int32_t value) {
  const uint32_t bits = static_cast<uint32_t>(value);
  const std::string key = std::format("c|i32|{}", bits);
  const auto it = constantIds_.find(key);
  if (it != constantIds_.end()) return it->second;
  const uint32_t typeId = typeScalar(ScalarKind::I32);
  const uint32_t id = newId();
  Instr(globals_, kOpConstant, {typeId, id, bits});
  constantIds_[key] = id;
  return id;
}

uint32_t Emitter::constF32(float value) {
  if (!std::isfinite(value)) {
    latch(std::format("non-finite float literal in function \"{}\" cannot be emitted as SPIR-V",
                      currentFunctionName_));
    return 0;
  }
  const uint32_t bits = std::bit_cast<uint32_t>(value);
  const std::string key = std::format("c|f32|{}", bits);
  const auto it = constantIds_.find(key);
  if (it != constantIds_.end()) return it->second;
  const uint32_t typeId = typeScalar(ScalarKind::F32);
  const uint32_t id = newId();
  Instr(globals_, kOpConstant, {typeId, id, bits});
  constantIds_[key] = id;
  return id;
}

uint32_t Emitter::constSplat(uint32_t vectorTypeId, uint32_t scalarId, uint32_t size) {
  const std::string key = std::format("cc|{}|{}|{}", vectorTypeId, scalarId, size);
  const auto it = constantIds_.find(key);
  if (it != constantIds_.end()) return it->second;
  const uint32_t id = newId();
  std::vector<uint32_t> operands = {vectorTypeId, id};
  for (uint32_t i = 0; i < size; ++i) {
    operands.push_back(scalarId);
  }
  InstrV(globals_, kOpConstantComposite, operands);
  constantIds_[key] = id;
  return id;
}

// ----- Module-scope emission -----

void Emitter::emitBindings() {
  for (const IrBinding& binding : module_.bindings()) {
    BindingInfo info;
    info.kind = binding.kind;
    info.type = binding.type;
    switch (binding.kind) {
      case BindingKind::UniformBuffer: {
        info.storageClass = kStorageClassUniform;
        const uint32_t rootId = blockStructId(binding.type, AddressSpace::Uniform);
        const uint32_t pointerId = typePointer(kStorageClassUniform, rootId);
        info.varId = newId();
        Instr(globals_, kOpVariable, {pointerId, info.varId, kStorageClassUniform});
        break;
      }
      case BindingKind::ReadOnlyStorageBuffer: {
        info.storageClass = kStorageClassStorageBuffer;
        uint32_t rootId = 0;
        if (binding.type.kind() == IrType::Kind::RuntimeArray) {
          const uint32_t arrayId = laidTypeId(binding.type, AddressSpace::Storage);
          rootId = runtimeArrayBlockId(arrayId);
          info.wrapped = true;
        } else {
          rootId = blockStructId(binding.type, AddressSpace::Storage);
        }
        const uint32_t pointerId = typePointer(kStorageClassStorageBuffer, rootId);
        info.varId = newId();
        Instr(globals_, kOpVariable, {pointerId, info.varId, kStorageClassStorageBuffer});
        Instr(decorations_, kOpDecorate, {info.varId, kDecorationNonWritable});
        break;
      }
      case BindingKind::SampledTexture2dF32: {
        info.storageClass = kStorageClassUniformConstant;
        const uint32_t pointerId = typePointer(kStorageClassUniformConstant, typeImage2dF32());
        info.varId = newId();
        Instr(globals_, kOpVariable, {pointerId, info.varId, kStorageClassUniformConstant});
        break;
      }
      case BindingKind::FilteringSampler: {
        info.storageClass = kStorageClassUniformConstant;
        const uint32_t pointerId = typePointer(kStorageClassUniformConstant, typeSampler());
        info.varId = newId();
        Instr(globals_, kOpVariable, {pointerId, info.varId, kStorageClassUniformConstant});
        break;
      }
    }
    Instr(decorations_, kOpDecorate, {info.varId, kDecorationDescriptorSet, binding.group});
    Instr(decorations_, kOpDecorate, {info.varId, kDecorationBinding, binding.binding});
    bindings_.insert_or_assign(binding.name.str(), std::move(info));
  }
}

void Emitter::emitModuleConstants() {
  for (const IrConstant& constant : module_.constants()) {
    const uint32_t id = emitLiteral(constant.value.node());
    moduleConstants_.insert_or_assign(constant.name.str(), id);
  }
}

// ----- Function emission -----

void Emitter::hoistFunctionVariables(const IrBlock& block) {
  for (const IrStmt& statement : block) {
    const IrStmt::Data& data = statement.data();
    const auto declareVar = [&](const IrStmt::Data& varData) {
      const uint32_t typeId = plainTypeId(*varData.declaredType);
      const uint32_t pointerId = typePointer(kStorageClassFunction, typeId);
      const uint32_t varId = newId();
      Instr(functions_, kOpVariable, {pointerId, varId, kStorageClassFunction});
      hoistedVars_.insert_or_assign(&varData, varId);
    };
    switch (statement.kind()) {
      case IrStmt::Kind::Var: declareVar(data); break;
      case IrStmt::Kind::For:
        if (data.init) {
          declareVar(data.init->data());
        }
        hoistFunctionVariables(data.body);
        break;
      case IrStmt::Kind::If:
        hoistFunctionVariables(data.body);
        hoistFunctionVariables(data.elseBody);
        break;
      default: break;
    }
  }
}

void Emitter::emitFunction(const IrFunction& function) {
  currentStage_ = function.stage;
  currentFunctionName_ = function.name.str();
  scopes_.clear();
  hoistedVars_.clear();
  loopStack_.clear();
  currentOutputVars_.clear();
  pushScope();

  if (function.stage == StageKind::None) {
    for (const IrParam& param : function.params) {
      if (param.type.kind() == IrType::Kind::Texture2dF32 ||
          param.type.kind() == IrType::Kind::Sampler) {
        latch(
            std::format("plain function \"{}\" declares a texture or sampler parameter \"{}\"; "
                        "opaque handles must be accessed through module-scope bindings",
                        function.name.str(), param.name.str()));
        return;
      }
    }
    std::vector<uint32_t> paramTypeIds;
    for (const IrParam& param : function.params) {
      paramTypeIds.push_back(plainTypeId(param.type));
    }
    const uint32_t returnTypeId =
        function.returnType ? plainTypeId(*function.returnType) : typeVoid();
    const uint32_t functionTypeId = typeFunction(returnTypeId, paramTypeIds);
    const uint32_t functionId = newId();
    Instr(functions_, kOpFunction,
          {returnTypeId, functionId, kFunctionControlNone, functionTypeId});
    for (size_t i = 0; i < function.params.size(); ++i) {
      const uint32_t paramId = newId();
      Instr(functions_, kOpFunctionParameter, {paramTypeIds[i], paramId});
      bindLocal(function.params[i].name, Local{RefKind::Param, paramId, function.params[i].type});
    }
    startBlock(newId());
    hoistFunctionVariables(function.body);
    emitBlock(function.body);
    if (!blockTerminated_) {
      if (function.returnType) {
        // The builder requires a top-level return, so an unterminated tail block is an
        // unreachable merge; it still needs a terminator.
        Instr(functions_, kOpUnreachable, {});
      } else {
        Instr(functions_, kOpReturn, {});
      }
    }
    Instr(functions_, kOpFunctionEnd, {});
    functionIds_.insert_or_assign(function.name.str(), FunctionInfo{functionId, returnTypeId});
  } else {
    // Stage IO variables in parameter-then-output order, then the function type and body.
    std::vector<uint32_t> interface;
    std::vector<uint32_t> inputVars;
    for (const IrParam& param : function.params) {
      const uint32_t typeId = plainTypeId(param.type);
      const uint32_t pointerId = typePointer(kStorageClassInput, typeId);
      const uint32_t varId = newId();
      Instr(globals_, kOpVariable, {pointerId, varId, kStorageClassInput});
      if (param.builtin) {
        const uint32_t builtinValue = *param.builtin == BuiltinInput::InstanceIndex
                                          ? kBuiltInInstanceIndex
                                          : kBuiltInFragCoord;
        Instr(decorations_, kOpDecorate, {varId, kDecorationBuiltIn, builtinValue});
      } else if (param.location) {
        Instr(decorations_, kOpDecorate, {varId, kDecorationLocation, *param.location});
      }
      const bool integerInput = (param.type.isScalar() || param.type.isVector()) &&
                                (param.type.scalarKind() == ScalarKind::I32 ||
                                 param.type.scalarKind() == ScalarKind::U32);
      if (function.stage == StageKind::Fragment && integerInput) {
        // WGSL requires integer fragment inputs to be flat-interpolated.
        Instr(decorations_, kOpDecorate, {varId, kDecorationFlat});
      }
      inputVars.push_back(varId);
      interface.push_back(varId);
    }
    for (const IrOutputMember& output : function.outputs) {
      const uint32_t typeId = plainTypeId(output.type);
      const uint32_t pointerId = typePointer(kStorageClassOutput, typeId);
      const uint32_t varId = newId();
      Instr(globals_, kOpVariable, {pointerId, varId, kStorageClassOutput});
      if (output.builtin) {
        Instr(decorations_, kOpDecorate, {varId, kDecorationBuiltIn, kBuiltInPosition});
      } else if (output.location) {
        Instr(decorations_, kOpDecorate, {varId, kDecorationLocation, *output.location});
      }
      currentOutputVars_.push_back(varId);
      interface.push_back(varId);
    }

    const uint32_t voidId = typeVoid();
    const uint32_t functionTypeId = typeFunction(voidId, {});
    const uint32_t functionId = newId();
    Instr(functions_, kOpFunction, {voidId, functionId, kFunctionControlNone, functionTypeId});
    startBlock(newId());
    hoistFunctionVariables(function.body);
    for (size_t i = 0; i < function.params.size(); ++i) {
      const uint32_t typeId = plainTypeId(function.params[i].type);
      const uint32_t valueId = newId();
      Instr(functions_, kOpLoad, {typeId, valueId, inputVars[i]});
      bindLocal(function.params[i].name, Local{RefKind::Param, valueId, function.params[i].type});
    }
    emitBlock(function.body);
    if (!blockTerminated_) {
      Instr(functions_, kOpReturn, {});
    }
    Instr(functions_, kOpFunctionEnd, {});

    const uint32_t executionModel =
        function.stage == StageKind::Vertex ? kExecutionModelVertex : kExecutionModelFragment;
    std::vector<uint32_t> entryOperands = {executionModel, functionId};
    const std::vector<uint32_t> nameWords = EncodeString(std::string_view(function.name));
    entryOperands.insert(entryOperands.end(), nameWords.begin(), nameWords.end());
    entryOperands.insert(entryOperands.end(), interface.begin(), interface.end());
    InstrV(entryPoints_, kOpEntryPoint, entryOperands);
    if (function.stage == StageKind::Fragment) {
      Instr(executionModes_, kOpExecutionMode, {functionId, kExecutionModeOriginUpperLeft});
    }
  }
  popScope();
}

// ----- Statements -----

void Emitter::emitBlock(const IrBlock& block) {
  for (const IrStmt& statement : block) {
    if (blockTerminated_) {
      // Statements after a terminator in the same IR block are unreachable; drop them.
      return;
    }
    emitStatement(statement);
  }
}

void Emitter::emitStatement(const IrStmt& statement) {
  const IrStmt::Data& data = statement.data();
  switch (statement.kind()) {
    case IrStmt::Kind::Let: {
      const uint32_t valueId = emitValue(data.exprs[0]);
      bindLocal(data.name, Local{RefKind::Let, valueId, data.exprs[0].type()});
      return;
    }
    case IrStmt::Kind::Var: {
      const auto it = hoistedVars_.find(&data);
      const uint32_t varId = it != hoistedVars_.end() ? it->second : 0;
      bindLocal(data.name, Local{RefKind::Var, varId, *data.declaredType});
      if (!data.exprs.empty()) {
        const uint32_t initId = emitValue(data.exprs[0]);
        Instr(functions_, kOpStore, {varId, initId});
      }
      return;
    }
    case IrStmt::Kind::Assign: emitAssign(data.exprs[0], data.exprs[1]); return;
    case IrStmt::Kind::If: {
      const uint32_t conditionId = emitValue(data.exprs[0]);
      const uint32_t thenLabel = newId();
      const uint32_t elseLabel = data.elseBody.empty() ? 0 : newId();
      const uint32_t mergeLabel = newId();
      Instr(functions_, kOpSelectionMerge, {mergeLabel, kSelectionControlNone});
      Instr(functions_, kOpBranchConditional,
            {conditionId, thenLabel, elseLabel != 0 ? elseLabel : mergeLabel});
      startBlock(thenLabel);
      pushScope();
      emitBlock(data.body);
      popScope();
      if (!blockTerminated_) {
        Instr(functions_, kOpBranch, {mergeLabel});
      }
      if (elseLabel != 0) {
        startBlock(elseLabel);
        pushScope();
        emitBlock(data.elseBody);
        popScope();
        if (!blockTerminated_) {
          Instr(functions_, kOpBranch, {mergeLabel});
        }
      }
      startBlock(mergeLabel);
      return;
    }
    case IrStmt::Kind::For: {
      pushScope();  // Scope of the loop variable.
      if (data.init) {
        const IrStmt::Data& initData = data.init->data();
        const auto it = hoistedVars_.find(&initData);
        const uint32_t varId = it != hoistedVars_.end() ? it->second : 0;
        bindLocal(initData.name, Local{RefKind::Var, varId, *initData.declaredType});
        const uint32_t initId = emitValue(initData.exprs[0]);
        Instr(functions_, kOpStore, {varId, initId});
      }
      const uint32_t headerLabel = newId();
      const uint32_t conditionLabel = data.exprs.empty() ? 0 : newId();
      const uint32_t bodyLabel = newId();
      const uint32_t continueLabel = newId();
      const uint32_t mergeLabel = newId();

      Instr(functions_, kOpBranch, {headerLabel});
      startBlock(headerLabel);
      Instr(functions_, kOpLoopMerge, {mergeLabel, continueLabel, kLoopControlNone});
      Instr(functions_, kOpBranch, {conditionLabel != 0 ? conditionLabel : bodyLabel});
      if (conditionLabel != 0) {
        startBlock(conditionLabel);
        const uint32_t conditionId = emitValue(data.exprs[0]);
        Instr(functions_, kOpBranchConditional, {conditionId, bodyLabel, mergeLabel});
      }
      startBlock(bodyLabel);
      loopStack_.emplace_back(mergeLabel, continueLabel);
      pushScope();
      emitBlock(data.body);
      popScope();
      loopStack_.pop_back();
      if (!blockTerminated_) {
        Instr(functions_, kOpBranch, {continueLabel});
      }
      startBlock(continueLabel);
      if (data.continuing) {
        const IrStmt::Data& continuingData = data.continuing->data();
        emitAssign(continuingData.exprs[0], continuingData.exprs[1]);
      }
      Instr(functions_, kOpBranch, {headerLabel});
      startBlock(mergeLabel);
      popScope();
      return;
    }
    case IrStmt::Kind::Break:
      Instr(functions_, kOpBranch, {loopStack_.back().first});
      blockTerminated_ = true;
      return;
    case IrStmt::Kind::Continue:
      Instr(functions_, kOpBranch, {loopStack_.back().second});
      blockTerminated_ = true;
      return;
    case IrStmt::Kind::Return: {
      if (currentStage_ != StageKind::None) {
        for (size_t i = 0; i < data.exprs.size(); ++i) {
          const uint32_t valueId = emitValue(data.exprs[i]);
          Instr(functions_, kOpStore, {currentOutputVars_[i], valueId});
        }
        Instr(functions_, kOpReturn, {});
      } else if (!data.exprs.empty()) {
        const uint32_t valueId = emitValue(data.exprs[0]);
        Instr(functions_, kOpReturnValue, {valueId});
      } else {
        Instr(functions_, kOpReturn, {});
      }
      blockTerminated_ = true;
      return;
    }
    case IrStmt::Kind::Discard:
      if (currentStage_ == StageKind::Vertex) {
        latch(std::format("discard cannot appear in vertex entry point \"{}\"",
                          currentFunctionName_));
        return;
      }
      Instr(functions_, kOpKill, {});
      blockTerminated_ = true;
      return;
  }
}

void Emitter::emitAssign(const IrExpr& lhs, const IrExpr& rhs) {
  if (!isPointerBacked(lhs)) {
    latch("assignment target is not a var-rooted access chain");
    return;
  }
  const Chain chain = emitChain(lhs);
  const uint32_t rhsId = emitValue(rhs);
  uint32_t pointerId = chain.base;
  if (!chain.indices.empty()) {
    const uint32_t pointeeId = plainTypeId(lhs.type());
    const uint32_t pointerTypeId = typePointer(chain.storageClass, pointeeId);
    pointerId = newId();
    std::vector<uint32_t> operands = {pointerTypeId, pointerId, chain.base};
    operands.insert(operands.end(), chain.indices.begin(), chain.indices.end());
    InstrV(functions_, kOpAccessChain, operands);
  }
  Instr(functions_, kOpStore, {pointerId, rhsId});
}

// ----- Pointer chains -----

bool Emitter::isPointerBacked(const IrExpr& expr) const {
  const IrExpr::Node& node = expr.node();
  switch (node.kind) {
    case IrExpr::Kind::Ref:
      if (node.refKind == RefKind::Var) {
        return true;
      }
      if (node.refKind == RefKind::Resource) {
        const auto it = bindings_.find(node.name.str());
        return it != bindings_.end() && (it->second.kind == BindingKind::UniformBuffer ||
                                         it->second.kind == BindingKind::ReadOnlyStorageBuffer);
      }
      return false;
    case IrExpr::Kind::Member:
    case IrExpr::Kind::Index: return isPointerBacked(node.children[0]);
    case IrExpr::Kind::Swizzle:
      return node.swizzle.size() == 1 && isPointerBacked(node.children[0]);
    default: return false;
  }
}

Emitter::Chain Emitter::emitChain(const IrExpr& expr) {
  const IrExpr::Node& node = expr.node();
  switch (node.kind) {
    case IrExpr::Kind::Ref: {
      if (node.refKind == RefKind::Var) {
        const Local* local = resolveLocal(node.name.str());
        Chain chain;
        chain.base = local != nullptr ? local->id : 0;
        chain.storageClass = kStorageClassFunction;
        return chain;
      }
      const auto it = bindings_.find(node.name.str());
      Chain chain;
      if (it != bindings_.end()) {
        chain.base = it->second.varId;
        chain.storageClass = it->second.storageClass;
        chain.space = it->second.kind == BindingKind::UniformBuffer ? AddressSpace::Uniform
                                                                    : AddressSpace::Storage;
        if (it->second.wrapped) {
          chain.indices.push_back(constU32(0));
        }
      }
      return chain;
    }
    case IrExpr::Kind::Member: {
      Chain chain = emitChain(node.children[0]);
      uint32_t memberIndex = 0;
      const std::span<const IrType::Member> members = node.children[0].type().structMembers();
      for (size_t i = 0; i < members.size(); ++i) {
        if (members[i].name == node.name) {
          memberIndex = static_cast<uint32_t>(i);
          break;
        }
      }
      chain.indices.push_back(constU32(memberIndex));
      return chain;
    }
    case IrExpr::Kind::Index: {
      Chain chain = emitChain(node.children[0]);
      chain.indices.push_back(emitValue(node.children[1]));
      return chain;
    }
    case IrExpr::Kind::Swizzle: {
      Chain chain = emitChain(node.children[0]);
      chain.indices.push_back(constU32(SwizzleComponentIndex(node.swizzle[0])));
      return chain;
    }
    default: latch("internal: emitChain on a non-chain expression"); return Chain{};
  }
}

uint32_t Emitter::loadThroughChain(const Chain& chain, const IrType& type) {
  if (type.kind() == IrType::Kind::RuntimeArray) {
    latch(std::format("a runtime-sized array cannot be loaded as a whole value (function \"{}\")",
                      currentFunctionName_));
    return 0;
  }

  uint32_t pointerId = chain.base;
  if (!chain.indices.empty()) {
    const uint32_t pointeeId = chain.space ? laidTypeId(type, *chain.space) : plainTypeId(type);
    const uint32_t pointerTypeId = typePointer(chain.storageClass, pointeeId);
    pointerId = newId();
    std::vector<uint32_t> operands = {pointerTypeId, pointerId, chain.base};
    operands.insert(operands.end(), chain.indices.begin(), chain.indices.end());
    InstrV(functions_, kOpAccessChain, operands);
  }

  if (chain.space &&
      (type.kind() == IrType::Kind::Struct || type.kind() == IrType::Kind::SizedArray)) {
    return rebuildFromBuffer(pointerId, type, chain.storageClass, *chain.space);
  }
  const uint32_t typeId = plainTypeId(type);
  const uint32_t valueId = newId();
  Instr(functions_, kOpLoad, {typeId, valueId, pointerId});
  return valueId;
}

uint32_t Emitter::rebuildFromBuffer(uint32_t pointerId, const IrType& type, uint32_t storageClass,
                                    AddressSpace space) {
  const auto loadPart = [&](uint32_t index, const IrType& partType) -> uint32_t {
    const uint32_t partPointerTypeId = typePointer(storageClass, laidTypeId(partType, space));
    const uint32_t partPointerId = newId();
    Instr(functions_, kOpAccessChain, {partPointerTypeId, partPointerId, pointerId, index});
    if (partType.kind() == IrType::Kind::Struct || partType.kind() == IrType::Kind::SizedArray) {
      return rebuildFromBuffer(partPointerId, partType, storageClass, space);
    }
    if (partType.kind() == IrType::Kind::RuntimeArray) {
      latch("a runtime-sized array cannot be loaded as a whole value");
      return 0;
    }
    const uint32_t partTypeId = plainTypeId(partType);
    const uint32_t partId = newId();
    Instr(functions_, kOpLoad, {partTypeId, partId, partPointerId});
    return partId;
  };

  std::vector<uint32_t> parts;
  if (type.kind() == IrType::Kind::Struct) {
    const std::span<const IrType::Member> members = type.structMembers();
    for (size_t i = 0; i < members.size(); ++i) {
      parts.push_back(loadPart(constU32(static_cast<uint32_t>(i)), members[i].type));
    }
  } else {
    for (uint32_t i = 0; i < type.arrayCount(); ++i) {
      parts.push_back(loadPart(constU32(i), type.elementType()));
    }
  }
  const uint32_t typeId = plainTypeId(type);
  const uint32_t valueId = newId();
  std::vector<uint32_t> operands = {typeId, valueId};
  operands.insert(operands.end(), parts.begin(), parts.end());
  InstrV(functions_, kOpCompositeConstruct, operands);
  return valueId;
}

// ----- Expressions -----

uint32_t Emitter::emitValue(const IrExpr& expr) {
  const IrExpr::Node& node = expr.node();
  switch (node.kind) {
    case IrExpr::Kind::Literal: return emitLiteral(node);
    case IrExpr::Kind::Ref: return emitRefValue(node);
    case IrExpr::Kind::Unary: {
      const uint32_t operandId = emitValue(node.children[0]);
      const uint32_t typeId = plainTypeId(node.type);
      const uint32_t valueId = newId();
      if (node.unaryOp == IrExpr::UnaryOp::Not) {
        Instr(functions_, kOpLogicalNot, {typeId, valueId, operandId});
      } else {
        const uint32_t opcode = node.type.scalarKind() == ScalarKind::F32 ? kOpFNegate : kOpSNegate;
        Instr(functions_, opcode, {typeId, valueId, operandId});
      }
      return valueId;
    }
    case IrExpr::Kind::Binary: return emitBinary(node);
    case IrExpr::Kind::Member: {
      if (isPointerBacked(expr)) {
        return loadThroughChain(emitChain(expr), node.type);
      }
      const uint32_t baseId = emitValue(node.children[0]);
      uint32_t memberIndex = 0;
      const std::span<const IrType::Member> members = node.children[0].type().structMembers();
      for (size_t i = 0; i < members.size(); ++i) {
        if (members[i].name == node.name) {
          memberIndex = static_cast<uint32_t>(i);
          break;
        }
      }
      const uint32_t typeId = plainTypeId(node.type);
      const uint32_t valueId = newId();
      Instr(functions_, kOpCompositeExtract, {typeId, valueId, baseId, memberIndex});
      return valueId;
    }
    case IrExpr::Kind::Swizzle: {
      if (node.swizzle.size() == 1) {
        if (isPointerBacked(expr)) {
          return loadThroughChain(emitChain(expr), node.type);
        }
        const uint32_t baseId = emitValue(node.children[0]);
        const uint32_t typeId = plainTypeId(node.type);
        const uint32_t valueId = newId();
        Instr(functions_, kOpCompositeExtract,
              {typeId, valueId, baseId, SwizzleComponentIndex(node.swizzle[0])});
        return valueId;
      }
      const uint32_t baseId = emitValue(node.children[0]);
      const uint32_t typeId = plainTypeId(node.type);
      const uint32_t valueId = newId();
      std::vector<uint32_t> operands = {typeId, valueId, baseId, baseId};
      for (const char component : node.swizzle) {
        operands.push_back(SwizzleComponentIndex(component));
      }
      InstrV(functions_, kOpVectorShuffle, operands);
      return valueId;
    }
    case IrExpr::Kind::Index: {
      if (isPointerBacked(expr)) {
        return loadThroughChain(emitChain(expr), node.type);
      }
      const IrExpr& indexExpr = node.children[1];
      const uint32_t baseId = emitValue(node.children[0]);
      if (indexExpr.node().kind == IrExpr::Kind::Literal) {
        const uint32_t literalIndex =
            std::holds_alternative<uint32_t>(indexExpr.node().literal)
                ? std::get<uint32_t>(indexExpr.node().literal)
                : static_cast<uint32_t>(std::get<int32_t>(indexExpr.node().literal));
        const uint32_t typeId = plainTypeId(node.type);
        const uint32_t valueId = newId();
        Instr(functions_, kOpCompositeExtract, {typeId, valueId, baseId, literalIndex});
        return valueId;
      }
      if (node.children[0].type().kind() == IrType::Kind::Vector) {
        const uint32_t indexId = emitValue(indexExpr);
        const uint32_t typeId = plainTypeId(node.type);
        const uint32_t valueId = newId();
        Instr(functions_, kOpVectorExtractDynamic, {typeId, valueId, baseId, indexId});
        return valueId;
      }
      latch(
          std::format("an array value not backed by a pointer cannot be indexed dynamically "
                      "(function \"{}\")",
                      currentFunctionName_));
      return 0;
    }
    case IrExpr::Kind::Construct: {
      std::vector<uint32_t> argIds;
      for (const IrExpr& child : node.children) {
        argIds.push_back(emitValue(child));
      }
      if (node.type.kind() == IrType::Kind::Vector && argIds.size() == 1 &&
          node.children[0].type().isScalar()) {
        return emitSplat(node.type, argIds[0]);
      }
      const uint32_t typeId = plainTypeId(node.type);
      const uint32_t valueId = newId();
      std::vector<uint32_t> operands = {typeId, valueId};
      operands.insert(operands.end(), argIds.begin(), argIds.end());
      InstrV(functions_, kOpCompositeConstruct, operands);
      return valueId;
    }
    case IrExpr::Kind::Convert: return emitConvert(node);
    case IrExpr::Kind::CallBuiltin: return emitBuiltinCall(node);
    case IrExpr::Kind::CallUser: {
      const auto it = functionIds_.find(node.name.str());
      if (it == functionIds_.end()) {
        latch(std::format("call to unknown function \"{}\"", node.name.str()));
        return 0;
      }
      std::vector<uint32_t> argIds;
      for (const IrExpr& child : node.children) {
        argIds.push_back(emitValue(child));
      }
      const uint32_t valueId = newId();
      std::vector<uint32_t> operands = {it->second.returnTypeId, valueId, it->second.id};
      operands.insert(operands.end(), argIds.begin(), argIds.end());
      InstrV(functions_, kOpFunctionCall, operands);
      return valueId;
    }
  }
  return 0;
}

uint32_t Emitter::emitLiteral(const IrExpr::Node& node) {
  if (std::holds_alternative<bool>(node.literal)) {
    return constBool(std::get<bool>(node.literal));
  }
  if (std::holds_alternative<int32_t>(node.literal)) {
    return constI32(std::get<int32_t>(node.literal));
  }
  if (std::holds_alternative<uint32_t>(node.literal)) {
    return constU32(std::get<uint32_t>(node.literal));
  }
  return constF32(std::get<float>(node.literal));
}

uint32_t Emitter::emitRefValue(const IrExpr::Node& node) {
  switch (node.refKind) {
    case RefKind::Param:
    case RefKind::Let: {
      const Local* local = resolveLocal(node.name.str());
      if (local == nullptr) {
        latch(std::format("reference to unknown local \"{}\"", node.name.str()));
        return 0;
      }
      return local->id;
    }
    case RefKind::Var: {
      const Local* local = resolveLocal(node.name.str());
      if (local == nullptr) {
        latch(std::format("reference to unknown var \"{}\"", node.name.str()));
        return 0;
      }
      const uint32_t typeId = plainTypeId(node.type);
      const uint32_t valueId = newId();
      Instr(functions_, kOpLoad, {typeId, valueId, local->id});
      return valueId;
    }
    case RefKind::Constant: {
      const auto it = moduleConstants_.find(node.name.str());
      if (it == moduleConstants_.end()) {
        latch(std::format("reference to unknown module constant \"{}\"", node.name.str()));
        return 0;
      }
      return it->second;
    }
    case RefKind::Resource: {
      const auto it = bindings_.find(node.name.str());
      if (it == bindings_.end()) {
        latch(std::format("reference to unknown binding \"{}\"", node.name.str()));
        return 0;
      }
      const BindingInfo& info = it->second;
      if (info.kind == BindingKind::SampledTexture2dF32 ||
          info.kind == BindingKind::FilteringSampler) {
        const uint32_t typeId =
            info.kind == BindingKind::SampledTexture2dF32 ? typeImage2dF32() : typeSampler();
        const uint32_t valueId = newId();
        Instr(functions_, kOpLoad, {typeId, valueId, info.varId});
        return valueId;
      }
      // Whole load of a buffer root (member-by-member rebuild; runtime arrays fail closed).
      Chain chain;
      chain.base = info.varId;
      chain.storageClass = info.storageClass;
      chain.space =
          info.kind == BindingKind::UniformBuffer ? AddressSpace::Uniform : AddressSpace::Storage;
      if (info.wrapped) {
        chain.indices.push_back(constU32(0));
      }
      return loadThroughChain(chain, info.type);
    }
  }
  return 0;
}

uint32_t Emitter::emitBinary(const IrExpr::Node& node) {
  const IrType& lhsType = node.children[0].type();
  const IrType& rhsType = node.children[1].type();

  // Matrix products.
  if (node.binaryOp == BinaryOp::Mul && lhsType.kind() == IrType::Kind::Matrix4x4f) {
    const uint32_t lhsId = emitValue(node.children[0]);
    const uint32_t rhsId = emitValue(node.children[1]);
    const uint32_t typeId = plainTypeId(node.type);
    const uint32_t valueId = newId();
    const uint32_t opcode =
        rhsType.kind() == IrType::Kind::Matrix4x4f ? kOpMatrixTimesMatrix : kOpMatrixTimesVector;
    Instr(functions_, opcode, {typeId, valueId, lhsId, rhsId});
    return valueId;
  }

  // Vector-scalar broadcast forms of * and /.
  const bool vectorScalar = lhsType.isVector() && rhsType.isScalar();
  const bool scalarVector = lhsType.isScalar() && rhsType.isVector();
  if ((node.binaryOp == BinaryOp::Mul || node.binaryOp == BinaryOp::Div) &&
      (vectorScalar || scalarVector)) {
    uint32_t lhsId = emitValue(node.children[0]);
    uint32_t rhsId = emitValue(node.children[1]);
    const uint32_t typeId = plainTypeId(node.type);
    if (node.binaryOp == BinaryOp::Mul && node.type.scalarKind() == ScalarKind::F32) {
      const uint32_t vectorId = vectorScalar ? lhsId : rhsId;
      const uint32_t scalarId = vectorScalar ? rhsId : lhsId;
      const uint32_t valueId = newId();
      Instr(functions_, kOpVectorTimesScalar, {typeId, valueId, vectorId, scalarId});
      return valueId;
    }
    // Splat the scalar side; division and integer multiplication are componentwise.
    if (vectorScalar) {
      rhsId = emitSplat(node.type, rhsId);
    } else {
      lhsId = emitSplat(node.type, lhsId);
    }
    uint32_t opcode = 0;
    if (node.binaryOp == BinaryOp::Mul) {
      opcode = node.type.scalarKind() == ScalarKind::F32 ? kOpFMul : kOpIMul;
    } else {
      switch (node.type.scalarKind()) {
        case ScalarKind::F32: opcode = kOpFDiv; break;
        case ScalarKind::I32: opcode = kOpSDiv; break;
        default: opcode = kOpUDiv; break;
      }
    }
    const uint32_t valueId = newId();
    Instr(functions_, opcode, {typeId, valueId, lhsId, rhsId});
    return valueId;
  }

  const uint32_t lhsId = emitValue(node.children[0]);
  const uint32_t rhsId = emitValue(node.children[1]);
  const uint32_t typeId = plainTypeId(node.type);
  const ScalarKind operandKind =
      lhsType.isScalar() || lhsType.isVector() ? lhsType.scalarKind() : ScalarKind::F32;

  uint32_t opcode = 0;
  switch (node.binaryOp) {
    case BinaryOp::Add: opcode = operandKind == ScalarKind::F32 ? kOpFAdd : kOpIAdd; break;
    case BinaryOp::Sub: opcode = operandKind == ScalarKind::F32 ? kOpFSub : kOpISub; break;
    case BinaryOp::Mul: opcode = operandKind == ScalarKind::F32 ? kOpFMul : kOpIMul; break;
    case BinaryOp::Div:
      switch (operandKind) {
        case ScalarKind::F32: opcode = kOpFDiv; break;
        case ScalarKind::I32: opcode = kOpSDiv; break;
        default: opcode = kOpUDiv; break;
      }
      break;
    case BinaryOp::Lt:
      switch (operandKind) {
        case ScalarKind::F32: opcode = kOpFOrdLessThan; break;
        case ScalarKind::I32: opcode = kOpSLessThan; break;
        default: opcode = kOpULessThan; break;
      }
      break;
    case BinaryOp::Le:
      switch (operandKind) {
        case ScalarKind::F32: opcode = kOpFOrdLessThanEqual; break;
        case ScalarKind::I32: opcode = kOpSLessThanEqual; break;
        default: opcode = kOpULessThanEqual; break;
      }
      break;
    case BinaryOp::Gt:
      switch (operandKind) {
        case ScalarKind::F32: opcode = kOpFOrdGreaterThan; break;
        case ScalarKind::I32: opcode = kOpSGreaterThan; break;
        default: opcode = kOpUGreaterThan; break;
      }
      break;
    case BinaryOp::Ge:
      switch (operandKind) {
        case ScalarKind::F32: opcode = kOpFOrdGreaterThanEqual; break;
        case ScalarKind::I32: opcode = kOpSGreaterThanEqual; break;
        default: opcode = kOpUGreaterThanEqual; break;
      }
      break;
    case BinaryOp::Eq:
      switch (operandKind) {
        case ScalarKind::F32: opcode = kOpFOrdEqual; break;
        case ScalarKind::Bool: opcode = kOpLogicalEqual; break;
        default: opcode = kOpIEqual; break;
      }
      break;
    case BinaryOp::Ne:
      switch (operandKind) {
        case ScalarKind::F32: opcode = kOpFOrdNotEqual; break;
        case ScalarKind::Bool: opcode = kOpLogicalNotEqual; break;
        default: opcode = kOpINotEqual; break;
      }
      break;
    // IR expressions are side-effect free, so eager logical ops match WGSL short-circuit
    // semantics.
    case BinaryOp::And: opcode = kOpLogicalAnd; break;
    case BinaryOp::Or: opcode = kOpLogicalOr; break;
  }
  const uint32_t valueId = newId();
  Instr(functions_, opcode, {typeId, valueId, lhsId, rhsId});
  return valueId;
}

uint32_t Emitter::emitConvert(const IrExpr::Node& node) {
  const IrType& sourceType = node.children[0].type();
  const ScalarKind sourceKind = sourceType.scalarKind();
  const ScalarKind targetKind = node.type.scalarKind();
  const uint32_t sourceId = emitValue(node.children[0]);

  // Scalar-to-vector conversions convert the scalar first, then splat.
  const bool splat = node.type.isVector() && sourceType.isScalar();
  const IrType convertTarget = splat ? IrType::Scalar(targetKind) : node.type;

  uint32_t convertedId = sourceId;
  if (sourceKind != targetKind) {
    uint32_t opcode = 0;
    if (sourceKind == ScalarKind::F32) {
      opcode = targetKind == ScalarKind::I32 ? kOpConvertFToS : kOpConvertFToU;
    } else if (targetKind == ScalarKind::F32) {
      opcode = sourceKind == ScalarKind::I32 ? kOpConvertSToF : kOpConvertUToF;
    } else {
      // i32 <-> u32 preserves the bit pattern (WGSL value conversion is modulo 2^32).
      opcode = kOpBitcast;
    }
    const uint32_t typeId = plainTypeId(convertTarget);
    convertedId = newId();
    Instr(functions_, opcode, {typeId, convertedId, sourceId});
  }
  if (splat) {
    return emitSplat(node.type, convertedId);
  }
  return convertedId;
}

uint32_t Emitter::emitExtInst(uint32_t typeId, uint32_t instruction,
                              std::vector<uint32_t> operands) {
  const uint32_t valueId = newId();
  std::vector<uint32_t> words = {typeId, valueId, glslImportId_, instruction};
  words.insert(words.end(), operands.begin(), operands.end());
  InstrV(functions_, kOpExtInst, words);
  return valueId;
}

uint32_t Emitter::emitSplat(const IrType& vectorType, uint32_t scalarId) {
  const uint32_t typeId = plainTypeId(vectorType);
  const uint32_t valueId = newId();
  std::vector<uint32_t> operands = {typeId, valueId};
  for (uint32_t i = 0; i < vectorType.vectorSize(); ++i) {
    operands.push_back(scalarId);
  }
  InstrV(functions_, kOpCompositeConstruct, operands);
  return valueId;
}

uint32_t Emitter::emitBuiltinCall(const IrExpr::Node& node) {
  const auto scalarKindOf = [](const IrType& type) { return type.scalarKind(); };
  const uint32_t typeId = plainTypeId(node.type);

  switch (node.builtin) {
    case BuiltinFn::Abs: {
      const uint32_t argId = emitValue(node.children[0]);
      switch (scalarKindOf(node.type)) {
        case ScalarKind::F32: return emitExtInst(typeId, kGlslFAbs, {argId});
        case ScalarKind::I32: return emitExtInst(typeId, kGlslSAbs, {argId});
        default: return argId;  // abs of an unsigned value is the identity.
      }
    }
    case BuiltinFn::Min:
    case BuiltinFn::Max: {
      const uint32_t lhsId = emitValue(node.children[0]);
      const uint32_t rhsId = emitValue(node.children[1]);
      uint32_t instruction = 0;
      if (node.builtin == BuiltinFn::Min) {
        switch (scalarKindOf(node.type)) {
          case ScalarKind::F32: instruction = kGlslFMin; break;
          case ScalarKind::I32: instruction = kGlslSMin; break;
          default: instruction = kGlslUMin; break;
        }
      } else {
        switch (scalarKindOf(node.type)) {
          case ScalarKind::F32: instruction = kGlslFMax; break;
          case ScalarKind::I32: instruction = kGlslSMax; break;
          default: instruction = kGlslUMax; break;
        }
      }
      return emitExtInst(typeId, instruction, {lhsId, rhsId});
    }
    case BuiltinFn::Clamp: {
      const uint32_t valueArg = emitValue(node.children[0]);
      const uint32_t lowArg = emitValue(node.children[1]);
      const uint32_t highArg = emitValue(node.children[2]);
      uint32_t instruction = 0;
      switch (scalarKindOf(node.type)) {
        case ScalarKind::F32: instruction = kGlslFClamp; break;
        case ScalarKind::I32: instruction = kGlslSClamp; break;
        default: instruction = kGlslUClamp; break;
      }
      return emitExtInst(typeId, instruction, {valueArg, lowArg, highArg});
    }
    case BuiltinFn::Saturate: {
      const uint32_t argId = emitValue(node.children[0]);
      uint32_t zeroId = constF32(0.0f);
      uint32_t oneId = constF32(1.0f);
      if (node.type.isVector()) {
        zeroId = constSplat(typeId, zeroId, node.type.vectorSize());
        oneId = constSplat(typeId, oneId, node.type.vectorSize());
      }
      return emitExtInst(typeId, kGlslFClamp, {argId, zeroId, oneId});
    }
    case BuiltinFn::Fract: return emitExtInst(typeId, kGlslFract, {emitValue(node.children[0])});
    case BuiltinFn::Sqrt: return emitExtInst(typeId, kGlslSqrt, {emitValue(node.children[0])});
    case BuiltinFn::Length: return emitExtInst(typeId, kGlslLength, {emitValue(node.children[0])});
    case BuiltinFn::Round:
      // WGSL round() mandates round-half-to-even; GLSL.std.450 Round leaves halfway cases
      // undefined, so RoundEven is the correct lowering.
      return emitExtInst(typeId, kGlslRoundEven, {emitValue(node.children[0])});
    case BuiltinFn::Fwidth: {
      if (currentStage_ == StageKind::Vertex) {
        latch(
            std::format("fwidth cannot appear in vertex entry point \"{}\"", currentFunctionName_));
        return 0;
      }
      const uint32_t argId = emitValue(node.children[0]);
      const uint32_t valueId = newId();
      Instr(functions_, kOpFwidth, {typeId, valueId, argId});
      return valueId;
    }
    case BuiltinFn::Select: {
      // IR order is select(falseValue, trueValue, condition); OpSelect takes the condition
      // first, then the true object, then the false object.
      const uint32_t falseId = emitValue(node.children[0]);
      const uint32_t trueId = emitValue(node.children[1]);
      uint32_t conditionId = emitValue(node.children[2]);
      if (node.type.isVector()) {
        // Before SPIR-V 1.4 a vector-result OpSelect requires a bool vector condition.
        const uint32_t boolVectorId = typeBoolVector(node.type.vectorSize());
        const uint32_t splatId = newId();
        std::vector<uint32_t> operands = {boolVectorId, splatId};
        for (uint32_t i = 0; i < node.type.vectorSize(); ++i) {
          operands.push_back(conditionId);
        }
        InstrV(functions_, kOpCompositeConstruct, operands);
        conditionId = splatId;
      }
      const uint32_t valueId = newId();
      Instr(functions_, kOpSelect, {typeId, valueId, conditionId, trueId, falseId});
      return valueId;
    }
    case BuiltinFn::TextureSample: {
      if (currentStage_ == StageKind::Vertex) {
        latch(std::format("textureSample cannot appear in vertex entry point \"{}\"",
                          currentFunctionName_));
        return 0;
      }
      const uint32_t imageId = emitValue(node.children[0]);
      const uint32_t samplerId = emitValue(node.children[1]);
      const uint32_t coordsId = emitValue(node.children[2]);
      const uint32_t sampledImageId = newId();
      Instr(functions_, kOpSampledImage, {typeSampledImage(), sampledImageId, imageId, samplerId});
      const uint32_t valueId = newId();
      Instr(functions_, kOpImageSampleImplicitLod, {typeId, valueId, sampledImageId, coordsId});
      return valueId;
    }
    case BuiltinFn::TextureLoad: {
      const uint32_t imageId = emitValue(node.children[0]);
      const uint32_t coordsId = emitValue(node.children[1]);
      const uint32_t levelId = emitValue(node.children[2]);
      const uint32_t valueId = newId();
      Instr(functions_, kOpImageFetch,
            {typeId, valueId, imageId, coordsId, kImageOperandsLodMask, levelId});
      return valueId;
    }
    case BuiltinFn::TextureDimensions: {
      usesImageQuery_ = true;
      const uint32_t imageId = emitValue(node.children[0]);
      const uint32_t valueId = newId();
      Instr(functions_, kOpImageQuerySizeLod, {typeId, valueId, imageId, constU32(0)});
      return valueId;
    }
  }
  return 0;
}

// ----- Assembly -----

ShaderResult<std::vector<uint32_t>> Emitter::emit() {
  glslImportId_ = newId();  // Always id 1, per the determinism contract.

  emitBindings();
  emitModuleConstants();
  for (const IrFunction& function : module_.functions()) {
    emitFunction(function);
    if (error_) {
      break;
    }
  }

  if (error_) {
    return *error_;
  }

  std::vector<uint32_t> out;
  out.push_back(kSpirvMagic);
  out.push_back(kSpirvVersion13);
  out.push_back(0);        // Generator magic (unregistered).
  out.push_back(nextId_);  // Id bound.
  out.push_back(0);        // Schema.

  Instr(out, kOpCapability, {kCapabilityShader});
  if (usesImageQuery_) {
    Instr(out, kOpCapability, {kCapabilityImageQuery});
  }
  {
    std::vector<uint32_t> operands = {glslImportId_};
    const std::vector<uint32_t> nameWords = EncodeString("GLSL.std.450");
    operands.insert(operands.end(), nameWords.begin(), nameWords.end());
    InstrV(out, kOpExtInstImport, operands);
  }
  Instr(out, kOpMemoryModel, {kAddressingModelLogical, kMemoryModelGlsl450});
  out.insert(out.end(), entryPoints_.begin(), entryPoints_.end());
  out.insert(out.end(), executionModes_.begin(), executionModes_.end());
  out.insert(out.end(), decorations_.begin(), decorations_.end());
  out.insert(out.end(), globals_.begin(), globals_.end());
  out.insert(out.end(), functions_.begin(), functions_.end());
  return out;
}

}  // namespace

ShaderResult<std::vector<uint32_t>> EmitSpirv(const IrModule& module) {
  return Emitter(module).emit();
}

}  // namespace donner::gpu::shader
