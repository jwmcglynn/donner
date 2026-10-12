#include "donner/gpu/shader/wgsl/SpirvEmitter.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/gpu/shader/wgsl/tests/ProjectionTestSupport.h"

namespace donner::gpu::shader::wgsl {
namespace {

using testing::ElementsAre;
using testing::Eq;
using testing::IsEmpty;
using testing::SizeIs;

constexpr std::string_view kSmallCompute = "@compute @workgroup_size(3,4,1) fn cs_main() {}";
constexpr uint32_t kGuardWord = 0xa5b6c7d8u;

/// Operand words of every instruction with this opcode, without assuming result-id allocation.
std::vector<std::vector<uint32_t>> OperandsOf(std::span<const uint32_t> words, uint32_t opcode) {
  std::vector<std::vector<uint32_t>> result;
  for (const auto& instruction : tests::Instructions(words)) {
    if (instruction.opcode == opcode) {
      result.push_back(instruction.operands);
    }
  }
  return result;
}

/// Decodes the SPIR-V literal string starting at one operand word.
std::string LiteralString(std::span<const uint32_t> operands, std::size_t first) {
  std::string result;
  for (std::size_t i = first; i < operands.size(); ++i) {
    for (uint32_t byte = 0; byte < 4; ++byte) {
      const char character = static_cast<char>((operands[i] >> (byte * 8u)) & 0xffu);
      if (character == '\0') {
        return result;
      }
      result.push_back(character);
    }
  }
  ADD_FAILURE() << "SPIR-V literal string lacks its terminating nul";
  return result;
}

TEST(SpirvEmitter, EveryCallerCapacityFailsClosedWithoutWritingPastItsRegion) {
  const ParseResult parsed = Parse(kSmallCompute);
  ASSERT_THAT(tests::Diagnose(kSmallCompute), Eq(tests::kAccepted));
  const std::vector<uint32_t> complete = tests::Spirv(kSmallCompute);
  ASSERT_THAT(complete, testing::Not(IsEmpty()));
  for (uint32_t capacity = 0; capacity <= complete.size() + 1u; ++capacity) {
    SCOPED_TRACE(capacity);
    std::vector<uint32_t> guarded(complete.size() + 3u, kGuardWord);
    SpirvSink sink{guarded.data() + 1, capacity};
    const SpirvEmitResult result = EmitSpirv(parsed.module, sink);
    EXPECT_THAT(guarded.front(), Eq(kGuardWord));
    EXPECT_THAT(std::span(guarded).subspan(capacity + 1u), testing::Each(Eq(kGuardWord)));
    if (capacity < complete.size()) {
      EXPECT_THAT(result.error, Eq(SpirvEmitError::Capacity));
      EXPECT_THAT(sink.error, Eq(SpirvEmitError::Capacity));
      EXPECT_THAT(sink.size, Eq(0u));
    } else {
      EXPECT_THAT(result.error, Eq(SpirvEmitError::None));
      EXPECT_THAT(sink.error, Eq(SpirvEmitError::None));
      EXPECT_THAT(sink.size, Eq(complete.size()));
      EXPECT_THAT(std::span(sink.data, sink.size), testing::ElementsAreArray(complete));
    }
  }
}

TEST(SpirvEmitter, CountingIgnoresPhysicalCapacityAndRecoveryResetsTheFailedSink) {
  const ParseResult parsed = Parse(kSmallCompute);
  ASSERT_THAT(tests::Diagnose(kSmallCompute), Eq(tests::kAccepted));
  const auto expected = tests::Spirv(kSmallCompute);
  ASSERT_THAT(expected, testing::Not(IsEmpty()));
  SpirvSink counting{nullptr, 1u};
  EXPECT_THAT(EmitSpirv(parsed.module, counting).error, Eq(SpirvEmitError::None));
  EXPECT_THAT(counting.size, Eq(expected.size()));
  std::vector<uint32_t> guarded(expected.size() + 2u, kGuardWord);
  SpirvSink sink{guarded.data() + 1, 1u};
  EXPECT_THAT(EmitSpirv(parsed.module, sink).error, Eq(SpirvEmitError::Capacity));
  EXPECT_THAT(sink.size, Eq(0u));
  sink.capacity = static_cast<uint32_t>(expected.size());
  EXPECT_THAT(EmitSpirv(parsed.module, sink).error, Eq(SpirvEmitError::None));
  EXPECT_THAT(sink.size, Eq(expected.size()));
  EXPECT_THAT(std::span(sink.data, sink.size), testing::ElementsAreArray(expected));
  EXPECT_THAT(guarded.front(), Eq(kGuardWord));
  EXPECT_THAT(guarded.back(), Eq(kGuardWord));
  EXPECT_THAT(EmitSpirv(parsed.module, counting).error, Eq(SpirvEmitError::None));
  EXPECT_THAT(counting.size, Eq(expected.size()));
}

TEST(SpirvEmitter, DirectSinkLatchesCapacityAndDetectsCountOverflowWithoutWrapping) {
  uint32_t guard = kGuardWord;
  SpirvSink sink{&guard, 0u};
  sink.word(7u);
  sink.word(9u);
  EXPECT_THAT(sink.error, Eq(SpirvEmitError::Capacity));
  EXPECT_THAT(sink.size, Eq(0u));
  EXPECT_THAT(guard, Eq(kGuardWord));
  SpirvSink counting{};
  counting.size = std::numeric_limits<uint32_t>::max();
  counting.word(7u);
  counting.word(9u);
  EXPECT_THAT(counting.error, Eq(SpirvEmitError::Capacity));
  EXPECT_THAT(counting.size, Eq(std::numeric_limits<uint32_t>::max()));
}

TEST(SpirvEmitter, EntryNamesKeepTheirNulPaddingAndWorkgroupModesAcrossWordBoundaries) {
  for (const std::string_view name : {"a", "abcd", "abcdefg", "abcdefgh"}) {
    SCOPED_TRACE(name);
    const std::string source = "@compute @workgroup_size(3,4,1) fn " + std::string(name) + "() {}";
    ASSERT_THAT(tests::Diagnose(source), Eq(tests::kAccepted));
    const auto words = tests::Spirv(source);
    ASSERT_THAT(words, SizeIs(testing::Ge(5u)));
    EXPECT_THAT(std::span(words).first(2), ElementsAre(0x07230203u, 0x00010300u));
    EXPECT_THAT(words[4], Eq(0u));
    const auto entries = OperandsOf(words, 15u);
    ASSERT_THAT(entries, SizeIs(1));
    ASSERT_THAT(entries.front(), SizeIs(3u + name.size() / 4u));
    EXPECT_THAT(entries.front()[0], Eq(5u));
    EXPECT_THAT(LiteralString(entries.front(), 2u), Eq(name));
    EXPECT_THAT(OperandsOf(words, 16u),
                ElementsAre(ElementsAre(entries.front()[1], 17u, 3u, 4u, 1u)));
    EXPECT_THAT(words[3], testing::Gt(entries.front()[1]));
  }
}

TEST(SpirvEmitter, UniformAndStorageBindingsRetainTheirStorageClassesAndAccessDecorations) {
  constexpr std::string_view source = R"wgsl(
struct Parameters { gain: vec4f, }
@group(0) @binding(2) var<uniform> uniformData: Parameters;
@group(0) @binding(5) var<storage,read> storageData: Parameters;
@group(0) @binding(7) var outputTexture: texture_storage_2d<rgba32float,write>;
@compute @workgroup_size(1) fn cs_main(@builtin(global_invocation_id) gid:vec3u) {
  textureStore(outputTexture,vec2i(gid.xy),uniformData.gain+storageData.gain);
}
)wgsl";
  ASSERT_THAT(tests::Diagnose(source), Eq(tests::kAccepted));
  const auto words = tests::Spirv(source);
  const auto decorations = OperandsOf(words, 71u);
  const auto variables = OperandsOf(words, 59u);
  for (const auto [binding, storage] : {std::pair{2u, 2u}, std::pair{5u, 12u}, std::pair{7u, 0u}}) {
    SCOPED_TRACE(binding);
    const auto descriptor = std::ranges::find_if(decorations, [binding](const auto& operands) {
      return operands.size() == 3u && operands[1] == 33u && operands[2] == binding;
    });
    ASSERT_THAT(decorations, testing::Contains(ElementsAre(testing::_, 33u, binding)));
    const uint32_t variable = descriptor->front();
    EXPECT_THAT(decorations, testing::Contains(ElementsAre(variable, 34u, 0u)));
    EXPECT_THAT(variables, testing::Contains(ElementsAre(testing::_, variable, storage)));
    if (binding == 5u) {
      EXPECT_THAT(decorations, testing::Contains(ElementsAre(variable, 24u)));
    } else if (binding == 7u) {
      EXPECT_THAT(decorations, testing::Contains(ElementsAre(variable, 25u)));
    } else {
      EXPECT_THAT(decorations, testing::Not(testing::Contains(ElementsAre(variable, 24u))));
    }
  }
}

TEST(SpirvEmitter, StorageImagesKeepTheirDeclaredFormatsAndWriteOnlyDecoration) {
  for (const auto [format, imageFormat] : {std::pair{std::string_view("rgba8unorm"), 4u},
                                           std::pair{std::string_view("rgba32float"), 1u}}) {
    SCOPED_TRACE(format);
    const std::string source = "@group(0) @binding(0) var outputTexture:texture_storage_2d<" +
                               std::string(format) +
                               ",write>;\n@compute @workgroup_size(1) fn cs_main() { "
                               "textureStore(outputTexture,vec2i(0),vec4f(0.5)); }";
    ASSERT_THAT(tests::Diagnose(source), Eq(tests::kAccepted));
    const auto words = tests::Spirv(source);
    EXPECT_THAT(OperandsOf(words, 25u),
                ElementsAre(ElementsAre(testing::_, testing::_, 1u, 0u, 0u, 0u, 2u, imageFormat)));
    EXPECT_THAT(OperandsOf(words, 71u), testing::Contains(ElementsAre(testing::_, 25u)));
    EXPECT_THAT(tests::Opcodes(words), testing::Contains(99u));
  }
}

TEST(SpirvEmitter, StorageMatrixLayoutKeepsEightByteColumnsAndDeclaredMemberOffsets) {
  constexpr std::string_view source = R"wgsl(
struct Parameters { basis:mat2x2f, bias:vec2f, }
@group(0) @binding(0) var<storage,read> parameters:Parameters;
fn apply(value:vec2f)->vec2f { return parameters.basis*value+parameters.bias; }
@group(0) @binding(1) var outputTexture:texture_storage_2d<rgba32float,write>;
@compute @workgroup_size(1) fn cs_main(@builtin(global_invocation_id) gid:vec3u) {
  textureStore(outputTexture,vec2i(gid.xy),vec4f(apply(vec2f(gid.xy)),0.0,1.0));
}
)wgsl";
  ASSERT_THAT(tests::Diagnose(source), Eq(tests::kAccepted));
  const auto words = tests::Spirv(source);
  const auto structures = OperandsOf(words, 30u);
  ASSERT_THAT(structures, SizeIs(1u));
  const uint32_t structure = structures.front()[0];
  EXPECT_THAT(OperandsOf(words, 72u),
              testing::IsSupersetOf({std::vector<uint32_t>{structure, 0u, 35u, 0u},
                                     std::vector<uint32_t>{structure, 1u, 35u, 16u},
                                     std::vector<uint32_t>{structure, 0u, 5u},
                                     std::vector<uint32_t>{structure, 0u, 7u, 8u}}));
  EXPECT_THAT(OperandsOf(words, 24u), ElementsAre(ElementsAre(testing::_, testing::_, 2u)));
  EXPECT_THAT(tests::Opcodes(words), testing::Contains(145u));
}

TEST(SpirvEmitter, BooleanLocalStructureHasNoHostBufferOffsets) {
  constexpr std::string_view helpers = R"wgsl(
struct Choice { enabled:bool, value:f32, }
fn evaluate(seed:f32)->f32 {
  let choice = Choice(seed > 0.0,seed);
  return select(0.0,choice.value,choice.enabled);
}
)wgsl";
  const std::string source = tests::ComputeModule(helpers, "evaluate(f32(gid.x))");
  ASSERT_THAT(tests::Diagnose(source), Eq(tests::kAccepted));
  const auto words = tests::Spirv(source);
  const auto booleans = OperandsOf(words, 20u);
  ASSERT_THAT(booleans, SizeIs(1u));
  const auto structures = OperandsOf(words, 30u);
  ASSERT_THAT(structures, SizeIs(1u));
  EXPECT_THAT(structures.front(), ElementsAre(testing::_, booleans.front()[0], testing::_));
  EXPECT_THAT(OperandsOf(words, 72u), IsEmpty());
}

TEST(SpirvEmitter, DereferencingALocalAddressUsesTheSameFunctionStorageVariable) {
  constexpr std::string_view helpers = R"wgsl(
fn evaluate(seed:f32)->f32 {
  var value = seed;
  *(&value) = *(&value) + 1.0;
  return *(&value);
}
)wgsl";
  const std::string source = tests::ComputeModule(helpers, "evaluate(f32(gid.x))");
  ASSERT_THAT(tests::Diagnose(source), Eq(tests::kAccepted));
  const auto words = tests::Spirv(source);
  const auto variables = OperandsOf(words, 59u);
  const auto local = std::ranges::find_if(
      variables, [](const auto& operands) { return operands.size() == 3u && operands[2] == 7u; });
  ASSERT_THAT(variables, testing::Contains(ElementsAre(testing::_, testing::_, 7u)));
  const uint32_t variable = (*local)[1];
  EXPECT_THAT(OperandsOf(words, 62u),
              testing::Contains(ElementsAre(variable, testing::_)).Times(2));
  EXPECT_THAT(OperandsOf(words, 61u),
              testing::Contains(ElementsAre(testing::_, testing::_, variable)).Times(2));
}

TEST(SpirvEmitter, BothReturningBranchesTerminateTheirUnreachableSelectionMerge) {
  constexpr std::string_view helpers = R"wgsl(
fn evaluate(seed:f32)->f32 {
  if (seed > 0.0) { return 1.0; } else { return 2.0; }
}
)wgsl";
  const std::string source = tests::ComputeModule(helpers, "evaluate(f32(gid.x))");
  ASSERT_THAT(tests::Diagnose(source), Eq(tests::kAccepted));
  const auto words = tests::Spirv(source);
  const auto entryPoints = OperandsOf(words, 15u);
  ASSERT_THAT(entryPoints, SizeIs(1u));
  const uint32_t entry = entryPoints.front()[1];
  std::vector<uint32_t> helperOpcodes;
  bool inHelper = false;
  for (const auto& instruction : tests::Instructions(words)) {
    if (instruction.opcode == 54u) {
      ASSERT_THAT(instruction.operands, SizeIs(4u));
      inHelper = instruction.operands[1] != entry;
    }
    if (inHelper) {
      helperOpcodes.push_back(instruction.opcode);
    }
    if (instruction.opcode == 56u) {
      inHelper = false;
    }
  }
  EXPECT_THAT(helperOpcodes, testing::Contains(255u));
  EXPECT_THAT(helperOpcodes, testing::Contains(254u).Times(2));
  EXPECT_THAT(helperOpcodes, testing::Contains(247u).Times(1));
}

}  // namespace
}  // namespace donner::gpu::shader::wgsl
