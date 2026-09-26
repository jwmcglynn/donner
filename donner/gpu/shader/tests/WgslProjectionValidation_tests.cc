/// @file
/// Validates every shipped WGSL projection without opening a native GPU adapter.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/tests/ShippedWgslCatalog.h"
#include "donner/gpu/shader/wgsl/Compiler.h"
#include "donner/gpu/shader/wgsl/Parser.h"
#include "donner/gpu/shader/wgsl/TextEmitter.h"

namespace donner::gpu::shader {
namespace {

struct InterfaceMismatch {
  std::string field;
  std::string expected;
  std::string actual;

  std::string describe() const { return field + ": expected " + expected + ", actual " + actual; }
};

template <typename T>
std::string FieldValue(const T& value) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<std::underlying_type_t<T>>(value));
  } else if constexpr (std::is_integral_v<T>) {
    return std::to_string(value);
  } else {
    return testing::PrintToString(value);
  }
}

template <typename T>
std::optional<InterfaceMismatch> CompareField(std::string_view field, const T& expected,
                                              const T& actual) {
  if (expected == actual) {
    return std::nullopt;
  }
  return InterfaceMismatch{std::string(field), FieldValue(expected), FieldValue(actual)};
}

std::optional<InterfaceMismatch> FirstMismatch(
    std::initializer_list<std::optional<InterfaceMismatch>> fields) {
  for (const auto& mismatch : fields) {
    if (mismatch) {
      return mismatch;
    }
  }
  return std::nullopt;
}

std::optional<InterfaceMismatch> CompareResource(const ShaderResource& expected,
                                                 const ShaderResource& actual) {
  return FirstMismatch({
      CompareField("name", expected.name.view(), actual.name.view()),
      CompareField("type", expected.type, actual.type),
      CompareField("group", expected.group, actual.group),
      CompareField("binding", expected.binding, actual.binding),
      CompareField("minSizeBytes", expected.minSizeBytes, actual.minSizeBytes),
      CompareField("alignmentBytes", expected.alignmentBytes, actual.alignmentBytes),
      CompareField("runtimeArrayStrideBytes", expected.runtimeArrayStrideBytes,
                   actual.runtimeArrayStrideBytes),
      CompareField("runtimeArrayScalarType", expected.runtimeArrayScalarType,
                   actual.runtimeArrayScalarType),
      CompareField("runtimeArrayLanes", expected.runtimeArrayLanes, actual.runtimeArrayLanes),
      CompareField("firstMember", expected.firstMember, actual.firstMember),
      CompareField("memberCount", expected.memberCount, actual.memberCount),
      CompareField("storageFormat", expected.storageFormat, actual.storageFormat),
  });
}

std::optional<InterfaceMismatch> CompareMember(const ShaderBufferMember& expected,
                                               const ShaderBufferMember& actual) {
  return FirstMismatch({
      CompareField("name", expected.name.view(), actual.name.view()),
      CompareField("scalarType", expected.scalarType, actual.scalarType),
      CompareField("lanes", expected.lanes, actual.lanes),
      CompareField("offsetBytes", expected.offsetBytes, actual.offsetBytes),
      CompareField("sizeBytes", expected.sizeBytes, actual.sizeBytes),
      CompareField("alignmentBytes", expected.alignmentBytes, actual.alignmentBytes),
      CompareField("arrayCount", expected.arrayCount, actual.arrayCount),
      CompareField("arrayStrideBytes", expected.arrayStrideBytes, actual.arrayStrideBytes),
      CompareField("matrixColumns", expected.matrixColumns, actual.matrixColumns),
      CompareField("matrixStrideBytes", expected.matrixStrideBytes, actual.matrixStrideBytes),
      CompareField("firstMember", expected.firstMember, actual.firstMember),
      CompareField("memberCount", expected.memberCount, actual.memberCount),
  });
}

std::optional<InterfaceMismatch> CompareEntry(const ShaderEntryPoint& expected,
                                              const ShaderEntryPoint& actual) {
  return FirstMismatch({
      CompareField("name", expected.name.view(), actual.name.view()),
      CompareField("stage", expected.stage, actual.stage),
      CompareField("workgroupSize", expected.workgroupSize, actual.workgroupSize),
      CompareField("firstInput", expected.firstInput, actual.firstInput),
      CompareField("inputCount", expected.inputCount, actual.inputCount),
      CompareField("firstOutput", expected.firstOutput, actual.firstOutput),
      CompareField("outputCount", expected.outputCount, actual.outputCount),
      CompareField("resourceMask", expected.resourceMask, actual.resourceMask),
  });
}

std::optional<InterfaceMismatch> CompareInterfaceVariable(const ShaderInterfaceVariable& expected,
                                                          const ShaderInterfaceVariable& actual) {
  return FirstMismatch({
      CompareField("name", expected.name.view(), actual.name.view()),
      CompareField("scalarType", expected.scalarType, actual.scalarType),
      CompareField("lanes", expected.lanes, actual.lanes),
      CompareField("builtin", expected.builtin, actual.builtin),
      CompareField("location", expected.location, actual.location),
      CompareField("flat", expected.flat, actual.flat),
  });
}

std::optional<InterfaceMismatch> InRecord(std::optional<InterfaceMismatch> mismatch,
                                          std::string_view collection, size_t index) {
  if (mismatch) {
    mismatch->field =
        std::string(collection) + "[" + std::to_string(index) + "]." + mismatch->field;
  }
  return mismatch;
}

std::optional<InterfaceMismatch> CompareCounts(const wgsl::Module& module,
                                               const CompiledShaderView& frozen) {
  return FirstMismatch({
      CompareField("resources.size", frozen.resources.size(),
                   static_cast<size_t>(module.bindingCount)),
      CompareField("members.size", frozen.members.size(),
                   static_cast<size_t>(module.structMemberCount)),
      CompareField("entryPoints.size", frozen.entryPoints.size(),
                   static_cast<size_t>(wgsl::compiler_detail::EntryCount(module))),
      CompareField("interfaceVariables.size", frozen.interfaceVariables.size(),
                   static_cast<size_t>(module.interfaceVariableCount)),
  });
}

std::optional<InterfaceMismatch> CompareResources(const wgsl::Module& module,
                                                  const CompiledShaderView& frozen) {
  for (uint16_t index = 0; index < module.bindingCount; ++index) {
    const ShaderResource parsed =
        wgsl::compiler_detail::ReflectResource(module, module.bindings[index]);
    if (auto mismatch =
            InRecord(CompareResource(frozen.resources[index], parsed), "resources", index)) {
      return mismatch;
    }
  }
  return std::nullopt;
}

std::optional<InterfaceMismatch> CompareMembers(const wgsl::Module& module,
                                                const CompiledShaderView& frozen) {
  for (uint16_t index = 0; index < module.structMemberCount; ++index) {
    const ShaderBufferMember parsed =
        wgsl::compiler_detail::ReflectMember(module, module.structMembers[index]);
    if (auto mismatch = InRecord(CompareMember(frozen.members[index], parsed), "members", index)) {
      return mismatch;
    }
  }
  return std::nullopt;
}

std::optional<InterfaceMismatch> CompareEntries(const wgsl::Module& module,
                                                const CompiledShaderView& frozen) {
  size_t entryIndex = 0;
  for (uint16_t index = 0; index < module.functionCount; ++index) {
    const wgsl::Function& function = module.functions[index];
    if (function.stage != wgsl::Stage::None) {
      const ShaderEntryPoint parsed = wgsl::compiler_detail::ReflectEntry(module, function);
      if (auto mismatch = InRecord(CompareEntry(frozen.entryPoints[entryIndex], parsed),
                                   "entryPoints", entryIndex)) {
        return mismatch;
      }
      ++entryIndex;
    }
  }
  return std::nullopt;
}

std::optional<InterfaceMismatch> CompareInterfaceVariables(const wgsl::Module& module,
                                                           const CompiledShaderView& frozen) {
  for (uint16_t index = 0; index < module.interfaceVariableCount; ++index) {
    const ShaderInterfaceVariable parsed =
        wgsl::compiler_detail::ReflectInterface(module, module.interfaceVariables[index]);
    if (auto mismatch = InRecord(CompareInterfaceVariable(frozen.interfaceVariables[index], parsed),
                                 "interfaceVariables", index)) {
      return mismatch;
    }
  }
  return std::nullopt;
}

std::optional<InterfaceMismatch> FindFrozenInterfaceMismatch(const wgsl::Module& module,
                                                             const CompiledShaderView& frozen) {
  if (auto mismatch = CompareCounts(module, frozen)) {
    return mismatch;
  }
  if (auto mismatch = CompareResources(module, frozen)) {
    return mismatch;
  }
  if (auto mismatch = CompareMembers(module, frozen)) {
    return mismatch;
  }
  if (auto mismatch = CompareEntries(module, frozen)) {
    return mismatch;
  }
  return CompareInterfaceVariables(module, frozen);
}

class ShippedWgslProjectionTest : public testing::TestWithParam<tests::ShippedWgslCase> {};

const CompiledShaderView* ShippedShader(std::string_view artifactTarget) {
  const std::span<const tests::ShippedWgslCase> catalog = tests::ShippedWgslCatalog();
  const auto match = std::find_if(catalog.begin(), catalog.end(), [&](const auto& entry) {
    return entry.artifactTarget == artifactTarget;
  });
  return match == catalog.end() ? nullptr : &match->shader();
}

TEST_P(ShippedWgslProjectionTest, ReparsePreservesCanonicalTextAndFrozenInterface) {
  const CompiledShaderView& frozen = GetParam().shader();
  ASSERT_FALSE(frozen.wgsl.empty());
  EXPECT_TRUE(frozen.msl.empty());
  EXPECT_TRUE(frozen.spirv.empty());

  const wgsl::ParseResult parsed = wgsl::Parse(frozen.wgsl);
  ASSERT_TRUE(parsed.hasResult()) << "Rejected shipped WGSL for " << GetParam().artifactTarget
                                  << " at bytes " << parsed.diagnostic.span.begin << "-"
                                  << parsed.diagnostic.span.end << " (error "
                                  << static_cast<int>(parsed.diagnostic.code) << ")";
  const std::optional<InterfaceMismatch> mismatch =
      FindFrozenInterfaceMismatch(parsed.module, frozen);
  EXPECT_FALSE(mismatch.has_value())
      << "Frozen host interface differs from parsed WGSL for " << GetParam().artifactTarget << ": "
      << (mismatch ? mismatch->describe() : "");

  std::vector<char> canonical(wgsl::kMaxTextEmitBytes);
  wgsl::TextSink sink{canonical.data(), static_cast<uint32_t>(canonical.size())};
  const wgsl::TextEmitResult emitted = wgsl::EmitWgsl(parsed.module, sink);
  ASSERT_TRUE(emitted.ok()) << "Could not re-emit " << GetParam().artifactTarget;
  EXPECT_EQ(sink.view(), frozen.wgsl)
      << "Shipped WGSL is not the canonical projection for " << GetParam().artifactTarget;
}

INSTANTIATE_TEST_SUITE_P(Production, ShippedWgslProjectionTest,
                         testing::ValuesIn(tests::ShippedWgslCatalog()),
                         [](const testing::TestParamInfo<tests::ShippedWgslCase>& info) {
                           return std::string(info.param.artifactTarget);
                         });

TEST(WgslProjectionValidation, RejectsInvalidWgsl) {
  const wgsl::ParseResult parsed = wgsl::Parse("fn broken( -> nonsense { this is not wgsl }");
  EXPECT_FALSE(parsed.hasResult());
  EXPECT_NE(parsed.diagnostic.code, wgsl::ErrorCode::None);
}

TEST(WgslProjectionValidation, DetectsResourceAndEntryPointMismatches) {
  const CompiledShaderView& frozen = tests::ShippedWgslCatalog().front().shader();
  ASSERT_FALSE(frozen.resources.empty());
  ASSERT_FALSE(frozen.entryPoints.empty());
  const wgsl::ParseResult parsed = wgsl::Parse(frozen.wgsl);
  ASSERT_TRUE(parsed.hasResult());
  ASSERT_FALSE(FindFrozenInterfaceMismatch(parsed.module, frozen).has_value());

  std::vector<ShaderResource> resources(frozen.resources.begin(), frozen.resources.end());
  resources.front().binding += 1;
  CompiledShaderView wrongResource = frozen;
  wrongResource.resources = resources;
  const std::optional<InterfaceMismatch> resourceMismatch =
      FindFrozenInterfaceMismatch(parsed.module, wrongResource);
  ASSERT_TRUE(resourceMismatch.has_value());
  EXPECT_EQ(resourceMismatch->field, "resources[0].binding");
  EXPECT_EQ(resourceMismatch->expected, std::to_string(resources.front().binding));
  EXPECT_EQ(resourceMismatch->actual, std::to_string(frozen.resources.front().binding));
  EXPECT_EQ(resourceMismatch->describe(), "resources[0].binding: expected " +
                                              resourceMismatch->expected + ", actual " +
                                              resourceMismatch->actual);

  std::vector<ShaderEntryPoint> entries(frozen.entryPoints.begin(), frozen.entryPoints.end());
  entries.front().stage =
      entries.front().stage == ShaderStage::Compute ? ShaderStage::Vertex : ShaderStage::Compute;
  CompiledShaderView wrongEntry = frozen;
  wrongEntry.entryPoints = entries;
  const std::optional<InterfaceMismatch> entryMismatch =
      FindFrozenInterfaceMismatch(parsed.module, wrongEntry);
  ASSERT_TRUE(entryMismatch.has_value());
  EXPECT_EQ(entryMismatch->field, "entryPoints[0].stage");
  EXPECT_EQ(entryMismatch->expected, FieldValue(entries.front().stage));
  EXPECT_EQ(entryMismatch->actual, FieldValue(frozen.entryPoints.front().stage));
}

TEST(WgslProjectionValidation, DetectsStorageFormatAccessAndSampleTypeMismatches) {
  const CompiledShaderView* flood = ShippedShader("flood_artifact");
  ASSERT_NE(flood, nullptr);
  const wgsl::ParseResult parsedFlood = wgsl::Parse(flood->wgsl);
  ASSERT_TRUE(parsedFlood.hasResult());
  ASSERT_FALSE(FindFrozenInterfaceMismatch(parsedFlood.module, *flood).has_value());

  std::vector<ShaderResource> floodResources(flood->resources.begin(), flood->resources.end());
  const auto output = std::find_if(
      floodResources.begin(), floodResources.end(),
      [](const ShaderResource& resource) { return resource.name.view() == "outputTexture"; });
  ASSERT_NE(output, floodResources.end());
  CompiledShaderView wrongFlood = *flood;
  wrongFlood.resources = floodResources;
  output->storageFormat = TextureFormat::RGBA8Unorm;
  const std::optional<InterfaceMismatch> formatMismatch =
      FindFrozenInterfaceMismatch(parsedFlood.module, wrongFlood);
  ASSERT_TRUE(formatMismatch.has_value());
  EXPECT_EQ(formatMismatch->field, "resources[0].storageFormat");
  EXPECT_EQ(formatMismatch->expected, FieldValue(TextureFormat::RGBA8Unorm));
  EXPECT_EQ(formatMismatch->actual, FieldValue(TextureFormat::RGBA32Float));
  output->storageFormat = TextureFormat::RGBA32Float;
  output->type = BindingType::SampledTexture2dFloat;
  const std::optional<InterfaceMismatch> accessMismatch =
      FindFrozenInterfaceMismatch(parsedFlood.module, wrongFlood);
  ASSERT_TRUE(accessMismatch.has_value());
  EXPECT_EQ(accessMismatch->field, "resources[0].type");
  EXPECT_EQ(accessMismatch->expected, FieldValue(BindingType::SampledTexture2dFloat));
  EXPECT_EQ(accessMismatch->actual, FieldValue(flood->resources.front().type));

  const CompiledShaderView* matrix = ShippedShader("filter_color_matrix_artifact");
  ASSERT_NE(matrix, nullptr);
  const wgsl::ParseResult parsedMatrix = wgsl::Parse(matrix->wgsl);
  ASSERT_TRUE(parsedMatrix.hasResult());
  ASSERT_FALSE(FindFrozenInterfaceMismatch(parsedMatrix.module, *matrix).has_value());
  std::vector<ShaderResource> matrixResources(matrix->resources.begin(), matrix->resources.end());
  const auto input = std::find_if(
      matrixResources.begin(), matrixResources.end(),
      [](const ShaderResource& resource) { return resource.name.view() == "inputTexture"; });
  ASSERT_NE(input, matrixResources.end());
  ASSERT_TRUE(input->type == BindingType::SampledTexture2dFloat ||
              input->type == BindingType::SampledTexture2dUnfilterableFloat);
  input->type = input->type == BindingType::SampledTexture2dFloat
                    ? BindingType::SampledTexture2dUnfilterableFloat
                    : BindingType::SampledTexture2dFloat;
  CompiledShaderView wrongMatrix = *matrix;
  wrongMatrix.resources = matrixResources;
  const std::optional<InterfaceMismatch> sampleMismatch =
      FindFrozenInterfaceMismatch(parsedMatrix.module, wrongMatrix);
  ASSERT_TRUE(sampleMismatch.has_value());
  EXPECT_EQ(sampleMismatch->field, "resources[0].type");
  EXPECT_EQ(sampleMismatch->expected, FieldValue(input->type));
  EXPECT_EQ(sampleMismatch->actual, FieldValue(matrix->resources.front().type));
}

}  // namespace
}  // namespace donner::gpu::shader
