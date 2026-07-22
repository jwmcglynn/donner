/// @file
/// SPIR-V emitter tests: module header and determinism, type dedup, structured control flow,
/// builtin lowerings, texture ops, entry point IO decorations, buffer layout decorations, the
/// committed solid-fill golden, and the fail-closed error paths.

#include "donner/gpu/shader/SpirvEmitter.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/gpu/shader/programs/SolidFill.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::Contains;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsSupersetOf;
using testing::SizeIs;

namespace donner::gpu::shader {
namespace {

// SPIR-V binary constants used by the assertions (from the public SPIR-V 1.3 specification).
constexpr uint32_t kSpirvMagic = 0x07230203;
constexpr uint32_t kSpirvVersion13 = 0x00010300;

constexpr uint32_t kOpExtInst = 12;
constexpr uint32_t kOpEntryPoint = 15;
constexpr uint32_t kOpExecutionMode = 16;
constexpr uint32_t kOpCapability = 17;
constexpr uint32_t kOpTypeInt = 21;
constexpr uint32_t kOpTypeFloat = 22;
constexpr uint32_t kOpTypeVector = 23;
constexpr uint32_t kOpTypeRuntimeArray = 29;
constexpr uint32_t kOpTypeStruct = 30;
constexpr uint32_t kOpTypePointer = 32;
constexpr uint32_t kOpConstantTrue = 41;
constexpr uint32_t kOpConstant = 43;
constexpr uint32_t kOpVariable = 59;
constexpr uint32_t kOpAccessChain = 65;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kOpMemberDecorate = 72;
constexpr uint32_t kOpCompositeConstruct = 80;
constexpr uint32_t kOpSampledImage = 86;
constexpr uint32_t kOpImageSampleImplicitLod = 87;
constexpr uint32_t kOpImageFetch = 95;
constexpr uint32_t kOpImageQuerySizeLod = 103;
constexpr uint32_t kOpSelect = 169;
constexpr uint32_t kOpFwidth = 209;
constexpr uint32_t kOpLoopMerge = 246;
constexpr uint32_t kOpSelectionMerge = 247;
constexpr uint32_t kOpLabel = 248;
constexpr uint32_t kOpBranch = 249;
constexpr uint32_t kOpBranchConditional = 250;

constexpr uint32_t kGlslRoundEven = 2;
constexpr uint32_t kGlslFClamp = 43;
constexpr uint32_t kGlslUClamp = 44;
constexpr uint32_t kGlslSClamp = 45;
constexpr uint32_t kGlslLength = 66;
constexpr uint32_t kGlslSqrt = 31;
constexpr uint32_t kGlslFract = 10;

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

constexpr uint32_t kBuiltInPosition = 0;
constexpr uint32_t kBuiltInFragCoord = 15;
constexpr uint32_t kBuiltInInstanceIndex = 43;

constexpr uint32_t kCapabilityShader = 1;
constexpr uint32_t kCapabilityImageQuery = 50;
constexpr uint32_t kExecutionModeOriginUpperLeft = 7;
constexpr uint32_t kExecutionModelVertex = 0;
constexpr uint32_t kExecutionModelFragment = 4;
constexpr uint32_t kStorageClassUniform = 2;
constexpr uint32_t kStorageClassStorageBuffer = 12;
constexpr uint32_t kImageOperandsLodMask = 0x2;

/// One decoded SPIR-V instruction: the opcode plus its operand words (the count/opcode word is
/// stripped).
struct SpvInstruction {
  uint32_t opcode = 0;             //!< Instruction opcode.
  std::vector<uint32_t> operands;  //!< Operand words in stream order.

  /// Equality operator. @param other Instruction to compare against.
  bool operator==(const SpvInstruction& other) const = default;

  /// Ostream output operator, e.g. `Op(43) [2, 7, 1065353216]`.
  /// @param os Output stream. @param instruction Instruction to output.
  friend std::ostream& operator<<(std::ostream& os, const SpvInstruction& instruction) {
    os << "Op(" << instruction.opcode << ") [";
    for (size_t i = 0; i < instruction.operands.size(); ++i) {
      os << (i > 0 ? ", " : "") << instruction.operands[i];
    }
    return os << "]";
  }
};

/// Decodes the word stream after the 5-word module header into instructions.
std::vector<SpvInstruction> Scan(const std::vector<uint32_t>& words) {
  std::vector<SpvInstruction> instructions;
  EXPECT_THAT(words, SizeIs(testing::Ge(5u))) << "missing SPIR-V module header";
  size_t index = 5;
  while (index < words.size()) {
    const uint32_t wordCount = words[index] >> 16;
    SpvInstruction instruction;
    instruction.opcode = words[index] & 0xFFFF;
    EXPECT_GE(wordCount, 1u) << "zero-length instruction at word " << index;
    EXPECT_LE(index + wordCount, words.size()) << "truncated instruction at word " << index;
    if (wordCount < 1 || index + wordCount > words.size()) {
      return instructions;
    }
    instruction.operands.assign(words.begin() + static_cast<ptrdiff_t>(index) + 1,
                                words.begin() + static_cast<ptrdiff_t>(index + wordCount));
    instructions.push_back(std::move(instruction));
    index += wordCount;
  }
  return instructions;
}

/// All instructions with the given opcode, in stream order.
std::vector<SpvInstruction> WithOpcode(const std::vector<SpvInstruction>& instructions,
                                       uint32_t opcode) {
  std::vector<SpvInstruction> result;
  for (const SpvInstruction& instruction : instructions) {
    if (instruction.opcode == opcode) {
      result.push_back(instruction);
    }
  }
  return result;
}

/// GLSL.std.450 instruction numbers of every OpExtInst in the module.
std::vector<uint32_t> ExtInstNumbers(const std::vector<SpvInstruction>& instructions) {
  std::vector<uint32_t> result;
  for (const SpvInstruction& instruction : WithOpcode(instructions, kOpExtInst)) {
    result.push_back(instruction.operands[3]);
  }
  return result;
}

/// Capability enum values declared by the module.
std::vector<uint32_t> Capabilities(const std::vector<SpvInstruction>& instructions) {
  std::vector<uint32_t> result;
  for (const SpvInstruction& instruction : WithOpcode(instructions, kOpCapability)) {
    result.push_back(instruction.operands[0]);
  }
  return result;
}

/// Decodes a nul-terminated literal string starting at \p wordIndex; returns the string and the
/// index of the first word after it.
std::pair<std::string, size_t> DecodeString(const std::vector<uint32_t>& operands,
                                            size_t wordIndex) {
  std::string text;
  size_t index = wordIndex;
  for (; index < operands.size(); ++index) {
    const uint32_t word = operands[index];
    for (int byte = 0; byte < 4; ++byte) {
      const char ch = static_cast<char>((word >> (8 * byte)) & 0xFF);
      if (ch == '\0') {
        return {text, index + 1};
      }
      text += ch;
    }
  }
  return {text, index};
}

/// The trailing literal operands of the first `OpDecorate target decoration ...` instruction, or
/// nullopt when the target does not carry the decoration.
std::optional<std::vector<uint32_t>> FindDecoration(const std::vector<SpvInstruction>& instructions,
                                                    uint32_t targetId, uint32_t decoration) {
  for (const SpvInstruction& instruction : WithOpcode(instructions, kOpDecorate)) {
    if (instruction.operands[0] == targetId && instruction.operands[1] == decoration) {
      return std::vector<uint32_t>(instruction.operands.begin() + 2, instruction.operands.end());
    }
  }
  return std::nullopt;
}

/// The trailing literal operands of `OpMemberDecorate struct member decoration ...`, or nullopt.
std::optional<std::vector<uint32_t>> FindMemberDecoration(
    const std::vector<SpvInstruction>& instructions, uint32_t structId, uint32_t member,
    uint32_t decoration) {
  for (const SpvInstruction& instruction : WithOpcode(instructions, kOpMemberDecorate)) {
    if (instruction.operands[0] == structId && instruction.operands[1] == member &&
        instruction.operands[2] == decoration) {
      return std::vector<uint32_t>(instruction.operands.begin() + 3, instruction.operands.end());
    }
  }
  return std::nullopt;
}

/// Finds the OpVariable decorated with (DescriptorSet \p group, Binding \p binding) and returns
/// {variableId, storageClass, pointeeTypeId}; fails the test when absent.
struct BindingVariable {
  uint32_t variableId = 0;    //!< OpVariable result id.
  uint32_t storageClass = 0;  //!< Storage class operand of the variable.
  uint32_t pointeeId = 0;     //!< Pointee type of the variable's pointer type.
};
std::optional<BindingVariable> FindBindingVariable(const std::vector<SpvInstruction>& instructions,
                                                   uint32_t group, uint32_t binding) {
  for (const SpvInstruction& variable : WithOpcode(instructions, kOpVariable)) {
    const uint32_t variableId = variable.operands[1];
    const std::optional<std::vector<uint32_t>> set =
        FindDecoration(instructions, variableId, kDecorationDescriptorSet);
    const std::optional<std::vector<uint32_t>> bindingDecoration =
        FindDecoration(instructions, variableId, kDecorationBinding);
    if (!set || !bindingDecoration || set->at(0) != group || bindingDecoration->at(0) != binding) {
      continue;
    }
    for (const SpvInstruction& pointer : WithOpcode(instructions, kOpTypePointer)) {
      if (pointer.operands[0] == variable.operands[0]) {
        return BindingVariable{variableId, variable.operands[2], pointer.operands[2]};
      }
    }
  }
  return std::nullopt;
}

/// Emits \p module, failing the test (and returning an empty stream) on error.
std::vector<uint32_t> EmitOrFail(const IrModule& module) {
  ShaderResult<std::vector<uint32_t>> result = EmitSpirv(module);
  EXPECT_THAT(result, HasShaderResult());
  if (result.hasError()) {
    return {};
  }
  return std::move(result).result();
}

IrModule BuildOrFail(ModuleBuilder& builder) {
  ShaderResult<IrModule> module = builder.build();
  EXPECT_THAT(module, HasShaderResult());
  return std::move(module).result();
}

// Unwrap helpers keeping module-construction code readable; construction failures are reported
// through the test, mirroring the WgslEmitter tests.
IrExpr V(ShaderResult<IrExpr>&& result) {
  return GetShaderResultOrFail(std::move(result), LiteralF32(0.0f));
}
FunctionBuilder Fn(ShaderResult<FunctionBuilder>&& result) {
  EXPECT_THAT(result, HasShaderResult());
  return std::move(result).result();
}
IrType T(ShaderResult<IrType>&& result) {
  return GetShaderResultOrFail(std::move(result), IrType::F32());
}
void OK(ShaderStatus&& status) {
  EXPECT_THAT(status, IsShaderOk());
}

IrModule BuildSolidFill() {
  ShaderResult<IrModule> module = programs::BuildSolidFillModule();
  EXPECT_THAT(module, HasShaderResult());
  return std::move(module).result();
}

/// Serializes SPIR-V words to the standard little-endian byte stream.
std::string WordsToBytes(const std::vector<uint32_t>& words) {
  std::string bytes;
  bytes.reserve(words.size() * 4);
  for (const uint32_t word : words) {
    bytes += static_cast<char>(word & 0xFF);
    bytes += static_cast<char>((word >> 8) & 0xFF);
    bytes += static_cast<char>((word >> 16) & 0xFF);
    bytes += static_cast<char>((word >> 24) & 0xFF);
  }
  return bytes;
}

// ----- Module header and determinism -----

TEST(SpirvEmitterTests, ModuleHeaderIsSpirv13ForVulkan) {
  const std::vector<uint32_t> words = EmitOrFail(BuildSolidFill());
  ASSERT_THAT(words, SizeIs(testing::Ge(5u)));
  EXPECT_THAT(words[0], testing::Eq(kSpirvMagic));
  EXPECT_THAT(words[1], testing::Eq(kSpirvVersion13));
  EXPECT_THAT(words[2], testing::Eq(0u)) << "generator magic should be unregistered (0)";
  EXPECT_THAT(words[3], testing::Ge(2u)) << "id bound";
  EXPECT_THAT(words[4], testing::Eq(0u)) << "reserved schema word";

  // Every result id must be below the declared bound (spot-check via decorations and types).
  const std::vector<SpvInstruction> instructions = Scan(words);
  for (const SpvInstruction& instruction : WithOpcode(instructions, kOpDecorate)) {
    EXPECT_THAT(instruction.operands[0], testing::Lt(words[3]));
  }
}

TEST(SpirvEmitterTests, EmitsDeterministically) {
  EXPECT_THAT(EmitOrFail(BuildSolidFill()), testing::Eq(EmitOrFail(BuildSolidFill())));
}

// ----- Type deduplication -----

TEST(SpirvEmitterTests, DeduplicatesScalarAndVectorTypes) {
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildSolidFill()));

  const std::vector<SpvInstruction> floats = WithOpcode(instructions, kOpTypeFloat);
  ASSERT_THAT(floats, SizeIs(1u)) << "f32 must be declared exactly once";
  const uint32_t floatId = floats[0].operands[0];

  // i32 and u32: exactly one OpTypeInt each.
  EXPECT_THAT(WithOpcode(instructions, kOpTypeInt), SizeIs(2u));

  int vec4fCount = 0;
  for (const SpvInstruction& vector : WithOpcode(instructions, kOpTypeVector)) {
    if (vector.operands[1] == floatId && vector.operands[2] == 4) {
      ++vec4fCount;
    }
  }
  EXPECT_THAT(vec4fCount, testing::Eq(1)) << "vec4<f32> must be declared exactly once";
}

// ----- Structured control flow -----

TEST(SpirvEmitterTests, IfElseUsesSelectionMerge) {
  ModuleBuilder builder;
  {
    auto fn = Fn(builder.createFunction("pick", {IrParam{"x", IrType::F32()}}, IrType::F32()));
    const IrExpr x = V(fn.ref("x"));
    const IrExpr r = V(fn.addVar("r", IrType::F32(), LiteralF32(0.0f)));
    OK(fn.beginIf(V(Lt(x, LiteralF32(0.0f)))));
    OK(fn.assign(r, LiteralF32(1.0f)));
    OK(fn.elseBranch());
    OK(fn.assign(r, LiteralF32(2.0f)));
    OK(fn.endIf());
    OK(fn.returnValue(r));
    OK(fn.finish());
  }
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildOrFail(builder)));

  const std::vector<SpvInstruction> merges = WithOpcode(instructions, kOpSelectionMerge);
  ASSERT_THAT(merges, SizeIs(1u));
  const uint32_t mergeLabel = merges[0].operands[0];

  const std::vector<SpvInstruction> conditionals = WithOpcode(instructions, kOpBranchConditional);
  ASSERT_THAT(conditionals, SizeIs(1u));
  const uint32_t thenLabel = conditionals[0].operands[1];
  const uint32_t elseLabel = conditionals[0].operands[2];
  EXPECT_THAT(thenLabel, testing::Not(testing::Eq(elseLabel)));
  EXPECT_THAT(thenLabel, testing::Not(testing::Eq(mergeLabel)));
  EXPECT_THAT(elseLabel, testing::Not(testing::Eq(mergeLabel)));

  // All three labels exist, and both branch arms end by branching to the merge block.
  std::vector<uint32_t> labels;
  for (const SpvInstruction& label : WithOpcode(instructions, kOpLabel)) {
    labels.push_back(label.operands[0]);
  }
  EXPECT_THAT(labels, IsSupersetOf({thenLabel, elseLabel, mergeLabel}));
  int branchesToMerge = 0;
  for (const SpvInstruction& branch : WithOpcode(instructions, kOpBranch)) {
    if (branch.operands[0] == mergeLabel) {
      ++branchesToMerge;
    }
  }
  EXPECT_THAT(branchesToMerge, testing::Eq(2))
      << "then and else arms must both branch to the merge block";
}

TEST(SpirvEmitterTests, ForLoopUsesLoopMergeWithBackEdge) {
  ModuleBuilder builder;
  {
    auto fn = Fn(builder.createFunction("total", {IrParam{"limit", IrType::U32()}}, IrType::U32()));
    const IrExpr limit = V(fn.ref("limit"));
    const IrExpr total = V(fn.addVar("total", IrType::U32(), LiteralU32(0)));
    const IrExpr i = V(fn.beginFor("i", LiteralU32(0)));
    OK(fn.forCondition(V(Lt(i, limit))));
    OK(fn.forContinuing(i, V(Add(i, LiteralU32(1)))));
    OK(fn.beginIf(V(Eq(i, LiteralU32(7)))));
    OK(fn.breakStmt());
    OK(fn.endIf());
    OK(fn.assign(total, V(Add(total, i))));
    OK(fn.endFor());
    OK(fn.returnValue(total));
    OK(fn.finish());
  }
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildOrFail(builder)));

  const std::vector<SpvInstruction> loopMerges = WithOpcode(instructions, kOpLoopMerge);
  ASSERT_THAT(loopMerges, SizeIs(1u));
  const uint32_t mergeLabel = loopMerges[0].operands[0];
  const uint32_t continueLabel = loopMerges[0].operands[1];
  EXPECT_THAT(mergeLabel, testing::Not(testing::Eq(continueLabel)));

  // The header block is the label immediately preceding OpLoopMerge; the continue block must
  // branch back to it (the loop back edge) and the preheader must branch into it.
  uint32_t headerLabel = 0;
  for (size_t index = 1; index < instructions.size(); ++index) {
    if (instructions[index].opcode == kOpLoopMerge && instructions[index - 1].opcode == kOpLabel) {
      headerLabel = instructions[index - 1].operands[0];
    }
  }
  ASSERT_THAT(headerLabel, testing::Not(testing::Eq(0u)));
  int branchesToHeader = 0;
  for (const SpvInstruction& branch : WithOpcode(instructions, kOpBranch)) {
    if (branch.operands[0] == headerLabel) {
      ++branchesToHeader;
    }
  }
  EXPECT_THAT(branchesToHeader, testing::Eq(2)) << "preheader entry plus continue-block back edge";

  // The loop condition branches to the body or the merge; `break` also branches to the merge.
  std::vector<SpvInstruction> conditionals = WithOpcode(instructions, kOpBranchConditional);
  bool loopConditionFound = false;
  for (const SpvInstruction& conditional : conditionals) {
    if (conditional.operands[2] == mergeLabel) {
      loopConditionFound = true;
    }
  }
  EXPECT_TRUE(loopConditionFound) << "loop condition must use the loop merge as its exit target";
}

// ----- Builtin lowerings -----

/// Module with one plain function calling the scalar math builtins with distinguishable
/// arguments.
IrModule BuildMathBuiltinModule() {
  ModuleBuilder builder;
  {
    auto fn =
        Fn(builder.createFunction("math",
                                  {IrParam{"x", IrType::F32()}, IrParam{"i", IrType::I32()},
                                   IrParam{"u", IrType::U32()}, IrParam{"v", IrType::Vec2f()}},
                                  IrType::F32()));
    const IrExpr x = V(fn.ref("x"));
    const IrExpr i = V(fn.ref("i"));
    const IrExpr u = V(fn.ref("u"));
    const IrExpr v = V(fn.ref("v"));
    V(fn.addLet("cf", V(CallBuiltin(BuiltinFn::Clamp, {x, LiteralF32(0.0f), LiteralF32(1.0f)}))));
    V(fn.addLet("ci", V(CallBuiltin(BuiltinFn::Clamp, {i, LiteralI32(0), LiteralI32(4)}))));
    V(fn.addLet("cu", V(CallBuiltin(BuiltinFn::Clamp, {u, LiteralU32(0), LiteralU32(4)}))));
    V(fn.addLet("r", V(CallBuiltin(BuiltinFn::Round, {x}))));
    V(fn.addLet("s", V(CallBuiltin(BuiltinFn::Saturate, {x}))));
    V(fn.addLet("q", V(CallBuiltin(BuiltinFn::Sqrt, {x}))));
    V(fn.addLet("f", V(CallBuiltin(BuiltinFn::Fract, {x}))));
    V(fn.addLet("len", V(CallBuiltin(BuiltinFn::Length, {v}))));
    OK(fn.returnValue(x));
    OK(fn.finish());
  }
  return BuildOrFail(builder);
}

TEST(SpirvEmitterTests, LowersMathBuiltinsToGlslStd450) {
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildMathBuiltinModule()));
  EXPECT_THAT(ExtInstNumbers(instructions),
              IsSupersetOf({kGlslFClamp, kGlslSClamp, kGlslUClamp, kGlslRoundEven, kGlslSqrt,
                            kGlslFract, kGlslLength}));
}

TEST(SpirvEmitterTests, RoundLowersToRoundEvenNotRound) {
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildMathBuiltinModule()));
  // GLSL.std.450 Round (1) leaves halfway cases undefined; WGSL round() mandates
  // round-half-to-even, so plain Round must never be emitted.
  EXPECT_THAT(ExtInstNumbers(instructions), testing::Not(Contains(1u)));
  EXPECT_THAT(ExtInstNumbers(instructions), Contains(kGlslRoundEven));
}

TEST(SpirvEmitterTests, SaturateLowersToFClampZeroOne) {
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildMathBuiltinModule()));
  // Find the f32 constants 0.0 and 1.0 (filtered by the f32 type so integer zeros do not
  // shadow them).
  const std::vector<SpvInstruction> floats = WithOpcode(instructions, kOpTypeFloat);
  ASSERT_THAT(floats, SizeIs(1u));
  const uint32_t floatTypeId = floats[0].operands[0];
  uint32_t zeroId = 0;
  uint32_t oneId = 0;
  for (const SpvInstruction& constant : WithOpcode(instructions, kOpConstant)) {
    if (constant.operands[0] != floatTypeId) continue;
    if (constant.operands[2] == 0x00000000) zeroId = constant.operands[1];
    if (constant.operands[2] == 0x3F800000) oneId = constant.operands[1];
  }
  ASSERT_THAT(zeroId, testing::Not(testing::Eq(0u)));
  ASSERT_THAT(oneId, testing::Not(testing::Eq(0u)));

  bool sawSaturateShape = false;
  for (const SpvInstruction& extInst : WithOpcode(instructions, kOpExtInst)) {
    if (extInst.operands[3] == kGlslFClamp && extInst.operands.size() == 7 &&
        extInst.operands[5] == zeroId && extInst.operands[6] == oneId) {
      sawSaturateShape = true;
    }
  }
  EXPECT_TRUE(sawSaturateShape) << "saturate(x) must lower to FClamp(x, 0.0, 1.0)";
}

TEST(SpirvEmitterTests, AbsOfU32IsIdentity) {
  ModuleBuilder builder;
  {
    auto fn = Fn(builder.createFunction("absu", {IrParam{"u", IrType::U32()}}, IrType::U32()));
    OK(fn.returnValue(V(CallBuiltin(BuiltinFn::Abs, {V(fn.ref("u"))}))));
    OK(fn.finish());
  }
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildOrFail(builder)));
  EXPECT_THAT(WithOpcode(instructions, kOpExtInst), SizeIs(0u))
      << "abs of an unsigned value must not emit FAbs/SAbs";
}

TEST(SpirvEmitterTests, SelectSwapsOperandOrderToConditionTrueFalse) {
  ModuleBuilder builder;
  {
    auto fn = Fn(builder.createFunction("sel", {}, IrType::F32()));
    // IR order: select(falseValue = 1.0, trueValue = 2.0, condition = true).
    OK(fn.returnValue(V(
        CallBuiltin(BuiltinFn::Select, {LiteralF32(1.0f), LiteralF32(2.0f), LiteralBool(true)}))));
    OK(fn.finish());
  }
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildOrFail(builder)));

  uint32_t oneId = 0;
  uint32_t twoId = 0;
  for (const SpvInstruction& constant : WithOpcode(instructions, kOpConstant)) {
    if (constant.operands[2] == 0x3F800000) oneId = constant.operands[1];
    if (constant.operands[2] == 0x40000000) twoId = constant.operands[1];
  }
  const std::vector<SpvInstruction> trues = WithOpcode(instructions, kOpConstantTrue);
  ASSERT_THAT(trues, SizeIs(1u));
  const uint32_t trueId = trues[0].operands[1];

  const std::vector<SpvInstruction> selects = WithOpcode(instructions, kOpSelect);
  ASSERT_THAT(selects, SizeIs(1u));
  // OpSelect operands: result type, result, condition, true object, false object.
  EXPECT_THAT(selects[0].operands, SizeIs(5u));
  EXPECT_THAT(selects[0].operands[2], testing::Eq(trueId))
      << "condition must be the third IR argument";
  EXPECT_THAT(selects[0].operands[3], testing::Eq(twoId))
      << "true object must be the second IR argument";
  EXPECT_THAT(selects[0].operands[4], testing::Eq(oneId))
      << "false object must be the first IR argument";
}

// ----- Texture builtins -----

IrModule BuildTextureModule() {
  ModuleBuilder builder;
  OK(builder.addTexture2d(0, 0, "tex"));
  OK(builder.addSampler(0, 1, "smp"));
  {
    auto fn = Fn(builder.createFragmentEntryPoint("fs_test", {IrParam{"uv", IrType::Vec2f(), 0}},
                                                  {IrOutputMember{"color", IrType::Vec4f(), 0}}));
    const IrExpr tex = V(fn.ref("tex"));
    const IrExpr smp = V(fn.ref("smp"));
    const IrExpr uv = V(fn.ref("uv"));
    V(fn.addLet("fw", V(CallBuiltin(BuiltinFn::Fwidth, {uv}))));
    const IrExpr sampled =
        V(fn.addLet("sampled", V(CallBuiltin(BuiltinFn::TextureSample, {tex, smp, uv}))));
    V(fn.addLet("dims", V(CallBuiltin(BuiltinFn::TextureDimensions, {tex}))));
    const IrExpr loaded = V(fn.addLet(
        "loaded",
        V(CallBuiltin(BuiltinFn::TextureLoad,
                      {tex, V(ConstructVector(IrType::Vec2i(), {LiteralI32(1), LiteralI32(2)})),
                       LiteralI32(0)}))));
    OK(fn.returnOutputs({V(Add(sampled, loaded))}));
    OK(fn.finish());
  }
  return BuildOrFail(builder);
}

TEST(SpirvEmitterTests, TextureSampleUsesSampledImageAndImplicitLod) {
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildTextureModule()));
  const std::vector<SpvInstruction> sampledImages = WithOpcode(instructions, kOpSampledImage);
  ASSERT_THAT(sampledImages, SizeIs(1u));
  const std::vector<SpvInstruction> samples = WithOpcode(instructions, kOpImageSampleImplicitLod);
  ASSERT_THAT(samples, SizeIs(1u));
  EXPECT_THAT(samples[0].operands[2], testing::Eq(sampledImages[0].operands[1]))
      << "the implicit-lod sample must consume the OpSampledImage result";
}

TEST(SpirvEmitterTests, FwidthLowersToOpFwidth) {
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildTextureModule()));
  EXPECT_THAT(WithOpcode(instructions, kOpFwidth), SizeIs(1u));
}

TEST(SpirvEmitterTests, TextureLoadUsesImageFetchWithLodOperand) {
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildTextureModule()));
  const std::vector<SpvInstruction> fetches = WithOpcode(instructions, kOpImageFetch);
  ASSERT_THAT(fetches, SizeIs(1u));
  // Operands: result type, result, image, coordinate, image operands mask, lod id.
  ASSERT_THAT(fetches[0].operands, SizeIs(6u));
  EXPECT_THAT(fetches[0].operands[4], testing::Eq(kImageOperandsLodMask));
}

TEST(SpirvEmitterTests, TextureDimensionsUsesQuerySizeLodAndImageQueryCapability) {
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildTextureModule()));
  EXPECT_THAT(WithOpcode(instructions, kOpImageQuerySizeLod), SizeIs(1u));
  EXPECT_THAT(Capabilities(instructions), ElementsAre(kCapabilityShader, kCapabilityImageQuery));
}

TEST(SpirvEmitterTests, ImageQueryCapabilityOmittedWithoutTextureDimensions) {
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildMathBuiltinModule()));
  EXPECT_THAT(Capabilities(instructions), ElementsAre(kCapabilityShader));
}

// ----- Entry point IO -----

IrModule BuildIoModule() {
  ModuleBuilder builder;
  {
    auto fn = Fn(builder.createVertexEntryPoint(
        "vs_test",
        {IrParam{"instance_index", IrType::U32(), std::nullopt, BuiltinInput::InstanceIndex},
         IrParam{"pos", IrType::Vec2f(), 0}},
        {IrOutputMember{"clip_pos", IrType::Vec4f(), std::nullopt, BuiltinOutput::Position},
         IrOutputMember{"uv", IrType::Vec2f(), 0}}));
    const IrExpr pos = V(fn.ref("pos"));
    OK(fn.returnOutputs(
        {V(ConstructVector(IrType::Vec4f(), {pos, LiteralF32(0.0f), LiteralF32(1.0f)})), pos}));
    OK(fn.finish());
  }
  {
    auto fn = Fn(builder.createFragmentEntryPoint(
        "fs_test",
        {IrParam{"frag_pos", IrType::Vec4f(), std::nullopt, BuiltinInput::Position},
         IrParam{"uv", IrType::Vec2f(), 0}, IrParam{"flag", IrType::U32(), 1}},
        {IrOutputMember{"color", IrType::Vec4f(), 0}}));
    const IrExpr uv = V(fn.ref("uv"));
    const IrExpr flag = V(fn.ref("flag"));
    const IrExpr scaled =
        V(Mul(V(ConstructVector(IrType::Vec4f(), {uv, LiteralF32(0.0f), LiteralF32(1.0f)})),
              V(Convert(IrType::F32(), flag))));
    OK(fn.returnOutputs({scaled}));
    OK(fn.finish());
  }
  return BuildOrFail(builder);
}

TEST(SpirvEmitterTests, EntryPointsDeclareInterfaceAndExecutionModes) {
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildIoModule()));
  const std::vector<SpvInstruction> entryPoints = WithOpcode(instructions, kOpEntryPoint);
  ASSERT_THAT(entryPoints, SizeIs(2u));

  EXPECT_THAT(entryPoints[0].operands[0], testing::Eq(kExecutionModelVertex));
  auto [vertexName, vertexInterfaceStart] = DecodeString(entryPoints[0].operands, 2);
  EXPECT_THAT(vertexName, testing::Eq("vs_test"));
  EXPECT_THAT(entryPoints[0].operands.size() - vertexInterfaceStart, testing::Eq(4u))
      << "vertex interface: 2 inputs + 2 outputs";

  EXPECT_THAT(entryPoints[1].operands[0], testing::Eq(kExecutionModelFragment));
  auto [fragmentName, fragmentInterfaceStart] = DecodeString(entryPoints[1].operands, 2);
  EXPECT_THAT(fragmentName, testing::Eq("fs_test"));
  EXPECT_THAT(entryPoints[1].operands.size() - fragmentInterfaceStart, testing::Eq(4u))
      << "fragment interface: 3 inputs + 1 output";

  // OriginUpperLeft applies to the fragment entry point only.
  const std::vector<SpvInstruction> modes = WithOpcode(instructions, kOpExecutionMode);
  ASSERT_THAT(modes, SizeIs(1u));
  EXPECT_THAT(modes[0].operands,
              ElementsAre(entryPoints[1].operands[1], kExecutionModeOriginUpperLeft));
}

TEST(SpirvEmitterTests, EntryPointIoCarriesBuiltinLocationAndFlatDecorations) {
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildIoModule()));

  // Collect (decoration, literal) pairs applied to any target.
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> decorations;
  for (const SpvInstruction& decoration : WithOpcode(instructions, kOpDecorate)) {
    decorations.emplace_back(
        decoration.operands[1],
        std::vector<uint32_t>(decoration.operands.begin() + 2, decoration.operands.end()));
  }
  EXPECT_THAT(
      decorations,
      IsSupersetOf({
          std::pair<uint32_t, std::vector<uint32_t>>{kDecorationBuiltIn, {kBuiltInInstanceIndex}},
          std::pair<uint32_t, std::vector<uint32_t>>{kDecorationBuiltIn, {kBuiltInPosition}},
          std::pair<uint32_t, std::vector<uint32_t>>{kDecorationBuiltIn, {kBuiltInFragCoord}},
      }));

  // The u32 fragment input at location 1 must be Flat; find the Location 1 target.
  uint32_t flagVarId = 0;
  for (const SpvInstruction& decoration : WithOpcode(instructions, kOpDecorate)) {
    if (decoration.operands[1] == kDecorationLocation && decoration.operands[2] == 1) {
      flagVarId = decoration.operands[0];
    }
  }
  ASSERT_THAT(flagVarId, testing::Not(testing::Eq(0u)));
  EXPECT_THAT(FindDecoration(instructions, flagVarId, kDecorationFlat),
              testing::Optional(ElementsAre()))
      << "integer fragment inputs must be flat-interpolated";
}

// ----- Buffer layout decorations -----

TEST(SpirvEmitterTests, UniformBlockCarriesOffsetsMatrixAndArrayStrideDecorations) {
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildSolidFill()));

  const std::optional<BindingVariable> uniforms = FindBindingVariable(instructions, 0, 0);
  ASSERT_TRUE(uniforms.has_value()) << "missing the (group 0, binding 0) uniform variable";
  EXPECT_THAT(uniforms->storageClass, testing::Eq(kStorageClassUniform));
  const uint32_t structId = uniforms->pointeeId;

  EXPECT_THAT(FindDecoration(instructions, structId, kDecorationBlock),
              testing::Optional(ElementsAre()));

  // Offsets from the IrLayout engine: mvp @0, patternFromPath @64, viewport @128,
  // clipPolygonPlanes @224 (member 21).
  EXPECT_THAT(FindMemberDecoration(instructions, structId, 0, kDecorationOffset),
              testing::Optional(ElementsAre(0u)));
  EXPECT_THAT(FindMemberDecoration(instructions, structId, 1, kDecorationOffset),
              testing::Optional(ElementsAre(64u)));
  EXPECT_THAT(FindMemberDecoration(instructions, structId, 2, kDecorationOffset),
              testing::Optional(ElementsAre(128u)));
  EXPECT_THAT(FindMemberDecoration(instructions, structId, 21, kDecorationOffset),
              testing::Optional(ElementsAre(224u)));

  // Matrix members carry ColMajor and MatrixStride 16.
  EXPECT_THAT(FindMemberDecoration(instructions, structId, 0, kDecorationColMajor),
              testing::Optional(ElementsAre()));
  EXPECT_THAT(FindMemberDecoration(instructions, structId, 0, kDecorationMatrixStride),
              testing::Optional(ElementsAre(16u)));

  // The clipPolygonPlanes member type is array<vec4f, 4> with uniform stride 16.
  const std::vector<SpvInstruction> structs = WithOpcode(instructions, kOpTypeStruct);
  uint32_t planesTypeId = 0;
  for (const SpvInstruction& structType : structs) {
    if (structType.operands[0] == structId) {
      planesTypeId = structType.operands.back();
    }
  }
  ASSERT_THAT(planesTypeId, testing::Not(testing::Eq(0u)));
  EXPECT_THAT(FindDecoration(instructions, planesTypeId, kDecorationArrayStride),
              testing::Optional(ElementsAre(16u)));
}

TEST(SpirvEmitterTests, StorageBufferGetsNonWritableAndSynthesizedBlockWrapper) {
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildSolidFill()));

  // Binding (0, 1) is `bands: array<Band>`.
  const std::optional<BindingVariable> bands = FindBindingVariable(instructions, 0, 1);
  ASSERT_TRUE(bands.has_value()) << "missing the (group 0, binding 1) storage variable";
  EXPECT_THAT(bands->storageClass, testing::Eq(kStorageClassStorageBuffer));
  EXPECT_THAT(FindDecoration(instructions, bands->variableId, kDecorationNonWritable),
              testing::Optional(ElementsAre()));

  // The pointee is a synthesized Block struct whose single member is the runtime array at
  // offset 0.
  uint32_t runtimeArrayId = 0;
  for (const SpvInstruction& structType : WithOpcode(instructions, kOpTypeStruct)) {
    if (structType.operands[0] == bands->pointeeId) {
      ASSERT_THAT(structType.operands, SizeIs(2u)) << "wrapper must have exactly one member";
      runtimeArrayId = structType.operands[1];
    }
  }
  ASSERT_THAT(runtimeArrayId, testing::Not(testing::Eq(0u)));
  EXPECT_THAT(FindDecoration(instructions, bands->pointeeId, kDecorationBlock),
              testing::Optional(ElementsAre()));
  EXPECT_THAT(FindMemberDecoration(instructions, bands->pointeeId, 0, kDecorationOffset),
              testing::Optional(ElementsAre(0u)));

  bool isRuntimeArray = false;
  for (const SpvInstruction& runtimeArray : WithOpcode(instructions, kOpTypeRuntimeArray)) {
    if (runtimeArray.operands[0] == runtimeArrayId) {
      isRuntimeArray = true;
    }
  }
  EXPECT_TRUE(isRuntimeArray) << "wrapper member 0 must be the runtime array";
  // Band is 32 bytes in the storage address space.
  EXPECT_THAT(FindDecoration(instructions, runtimeArrayId, kDecorationArrayStride),
              testing::Optional(ElementsAre(32u)));
}

TEST(SpirvEmitterTests, WholeStructLoadFromBufferRebuildsMemberByMember) {
  ModuleBuilder builder;
  const IrType pairType = T(IrType::Struct("Pair", {{"a", IrType::F32()}, {"b", IrType::F32()}}));
  OK(builder.addReadOnlyStorageBuffer(0, 0, "pair", pairType));
  {
    auto fn = Fn(builder.createFunction("readA", {}, IrType::F32()));
    const IrExpr p = V(fn.addLet("p", V(fn.ref("pair"))));
    OK(fn.returnValue(V(Member(p, "a"))));
    OK(fn.finish());
  }
  const std::vector<SpvInstruction> instructions = Scan(EmitOrFail(BuildOrFail(builder)));

  // The whole-struct load must access-chain to each member and rebuild the plain struct value
  // (SPIR-V 1.3 has no OpCopyLogical); the member read then extracts from the rebuilt value.
  EXPECT_THAT(WithOpcode(instructions, kOpAccessChain), SizeIs(2u));
  const std::vector<SpvInstruction> constructs = WithOpcode(instructions, kOpCompositeConstruct);
  ASSERT_THAT(constructs, SizeIs(1u));
  EXPECT_THAT(constructs[0].operands, SizeIs(4u)) << "type, result, and one id per member";
}

// ----- Golden -----

TEST(SpirvEmitterTests, SolidFillMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_SPIRV_GOLDEN=/path/to/repo rewrites the golden.
  const std::string bytes = WordsToBytes(EmitOrFail(BuildSolidFill()));

  if (const char* updateRoot = std::getenv("UPDATE_SPIRV_GOLDEN")) {
    const std::string outPath =
        std::string(updateRoot) + "/donner/gpu/shader/tests/testdata/solid_fill.spv";
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "Failed to open " << outPath << " for writing";
    out << bytes;
    GTEST_SKIP() << "Golden updated at " << outPath;
  }

  const std::string path =
      donner::Runfiles::instance().Rlocation("donner/gpu/shader/tests/testdata/solid_fill.spv");
  std::ifstream stream(path, std::ios::binary);
  ASSERT_TRUE(stream.good()) << "Failed to open golden file: " << path;
  std::ostringstream golden;
  golden << stream.rdbuf();

  EXPECT_THAT(bytes, testing::Eq(golden.str()));
}

// ----- Error paths -----

TEST(SpirvEmitterTests, RejectsNonFiniteFloatLiterals) {
  ModuleBuilder builder;
  {
    auto fn = Fn(builder.createFunction("bad", {}, IrType::F32()));
    V(fn.addLet("inf", LiteralF32(std::numeric_limits<float>::infinity())));
    OK(fn.returnValue(LiteralF32(0.0f)));
    OK(fn.finish());
  }
  EXPECT_THAT(EmitSpirv(BuildOrFail(builder)), IsShaderError(HasSubstr("non-finite")));
}

TEST(SpirvEmitterTests, RejectsWholeLoadOfRuntimeSizedArray) {
  ModuleBuilder builder;
  const IrType valuesType = T(IrType::RuntimeArray(IrType::F32()));
  OK(builder.addReadOnlyStorageBuffer(0, 0, "values", valuesType));
  {
    auto fn = Fn(builder.createFunction("bad", {}, IrType::F32()));
    V(fn.addLet("all", V(fn.ref("values"))));
    OK(fn.returnValue(LiteralF32(0.0f)));
    OK(fn.finish());
  }
  EXPECT_THAT(EmitSpirv(BuildOrFail(builder)), IsShaderError(HasSubstr("runtime-sized array")));
}

TEST(SpirvEmitterTests, RejectsDynamicIndexOfArrayRvalue) {
  ModuleBuilder builder;
  const IrType tapsType = T(IrType::SizedArray(IrType::F32(), 4));
  const IrType wrapType = T(IrType::Struct("Wrap", {{"taps", tapsType}}));
  OK(builder.addReadOnlyStorageBuffer(0, 0, "wrap", wrapType));
  {
    auto fn = Fn(builder.createFunction("bad", {IrParam{"idx", IrType::U32()}}, IrType::F32()));
    const IrExpr w = V(fn.addLet("w", V(fn.ref("wrap"))));
    OK(fn.returnValue(V(Index(V(Member(w, "taps")), V(fn.ref("idx"))))));
    OK(fn.finish());
  }
  EXPECT_THAT(EmitSpirv(BuildOrFail(builder)), IsShaderError(HasSubstr("indexed dynamically")));
}

TEST(SpirvEmitterTests, RejectsTextureParameterOnPlainFunction) {
  ModuleBuilder builder;
  {
    auto fn =
        Fn(builder.createFunction("helper", {IrParam{"t", IrType::Texture2dF32()}}, IrType::F32()));
    OK(fn.returnValue(LiteralF32(0.0f)));
    OK(fn.finish());
  }
  EXPECT_THAT(EmitSpirv(BuildOrFail(builder)),
              IsShaderError(HasSubstr("texture or sampler parameter")));
}

TEST(SpirvEmitterTests, RejectsBoolUniformMemberViaLayoutEngine) {
  ModuleBuilder builder;
  const IrType badType = T(IrType::Struct("BadUniforms", {{"flag", IrType::Bool()}}));
  OK(builder.addUniformBuffer(0, 0, "badUniforms", badType));
  EXPECT_THAT(EmitSpirv(BuildOrFail(builder)), IsShaderError(HasSubstr("bool")));
}

TEST(SpirvEmitterTests, FwidthInVertexStageFailsClosed) {
  // The IR builder rejects fragment-only builtins in vertex entry points at finish(); the
  // emitter's own stage check is defense in depth behind the same contract, so the fail-closed
  // property is asserted end-to-end here.
  ModuleBuilder builder;
  auto fn = Fn(builder.createVertexEntryPoint(
      "vs_bad", {IrParam{"pos", IrType::Vec2f(), 0}},
      {IrOutputMember{"clip_pos", IrType::Vec4f(), std::nullopt, BuiltinOutput::Position}}));
  const IrExpr pos = V(fn.ref("pos"));
  V(fn.addLet("w", V(CallBuiltin(BuiltinFn::Fwidth, {pos}))));
  OK(fn.returnOutputs(
      {V(ConstructVector(IrType::Vec4f(), {pos, LiteralF32(0.0f), LiteralF32(1.0f)}))}));
  EXPECT_THAT(fn.finish(), IsShaderError(HasSubstr("fragment-only")));
}

}  // namespace
}  // namespace donner::gpu::shader
