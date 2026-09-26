/// @file
/// Validates every shipped WGSL projection without opening a native GPU adapter.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/tests/ShippedWgslCatalog.h"
#include "donner/gpu/shader/wgsl/Compiler.h"
#include "donner/gpu/shader/wgsl/Parser.h"
#include "donner/gpu/shader/wgsl/TextEmitter.h"

namespace donner::gpu::shader {
namespace {

bool MatchesResource(const ShaderResource& frozen, const ShaderResource& parsed) {
  return frozen.name.view() == parsed.name.view() && frozen.type == parsed.type &&
         frozen.group == parsed.group && frozen.binding == parsed.binding &&
         frozen.minSizeBytes == parsed.minSizeBytes &&
         frozen.alignmentBytes == parsed.alignmentBytes &&
         frozen.runtimeArrayStrideBytes == parsed.runtimeArrayStrideBytes &&
         frozen.runtimeArrayScalarType == parsed.runtimeArrayScalarType &&
         frozen.runtimeArrayLanes == parsed.runtimeArrayLanes &&
         frozen.firstMember == parsed.firstMember && frozen.memberCount == parsed.memberCount &&
         frozen.storageFormat == parsed.storageFormat;
}

bool MatchesMember(const ShaderBufferMember& frozen, const ShaderBufferMember& parsed) {
  return frozen.name.view() == parsed.name.view() && frozen.scalarType == parsed.scalarType &&
         frozen.lanes == parsed.lanes && frozen.offsetBytes == parsed.offsetBytes &&
         frozen.sizeBytes == parsed.sizeBytes && frozen.alignmentBytes == parsed.alignmentBytes &&
         frozen.arrayCount == parsed.arrayCount &&
         frozen.arrayStrideBytes == parsed.arrayStrideBytes &&
         frozen.matrixColumns == parsed.matrixColumns &&
         frozen.matrixStrideBytes == parsed.matrixStrideBytes &&
         frozen.firstMember == parsed.firstMember && frozen.memberCount == parsed.memberCount;
}

bool MatchesEntry(const ShaderEntryPoint& frozen, const ShaderEntryPoint& parsed) {
  return frozen.name.view() == parsed.name.view() && frozen.stage == parsed.stage &&
         frozen.workgroupSize == parsed.workgroupSize && frozen.firstInput == parsed.firstInput &&
         frozen.inputCount == parsed.inputCount && frozen.firstOutput == parsed.firstOutput &&
         frozen.outputCount == parsed.outputCount && frozen.resourceMask == parsed.resourceMask;
}

bool MatchesInterfaceVariable(const ShaderInterfaceVariable& frozen,
                              const ShaderInterfaceVariable& parsed) {
  return frozen.name.view() == parsed.name.view() && frozen.scalarType == parsed.scalarType &&
         frozen.lanes == parsed.lanes && frozen.builtin == parsed.builtin &&
         frozen.location == parsed.location && frozen.flat == parsed.flat;
}

bool MatchesFrozenInterface(const wgsl::Module& module, const CompiledShaderView& frozen) {
  if (module.bindingCount != frozen.resources.size() ||
      module.structMemberCount != frozen.members.size() ||
      wgsl::compiler_detail::EntryCount(module) != frozen.entryPoints.size() ||
      module.interfaceVariableCount != frozen.interfaceVariables.size()) {
    return false;
  }
  for (uint16_t index = 0; index < module.bindingCount; ++index) {
    if (!MatchesResource(frozen.resources[index],
                         wgsl::compiler_detail::ReflectResource(module, module.bindings[index]))) {
      return false;
    }
  }
  for (uint16_t index = 0; index < module.structMemberCount; ++index) {
    if (!MatchesMember(frozen.members[index],
                       wgsl::compiler_detail::ReflectMember(module, module.structMembers[index]))) {
      return false;
    }
  }
  size_t entryIndex = 0;
  for (uint16_t index = 0; index < module.functionCount; ++index) {
    const wgsl::Function& function = module.functions[index];
    if (function.stage != wgsl::Stage::None &&
        !MatchesEntry(frozen.entryPoints[entryIndex++],
                      wgsl::compiler_detail::ReflectEntry(module, function))) {
      return false;
    }
  }
  for (uint16_t index = 0; index < module.interfaceVariableCount; ++index) {
    if (!MatchesInterfaceVariable(
            frozen.interfaceVariables[index],
            wgsl::compiler_detail::ReflectInterface(module, module.interfaceVariables[index]))) {
      return false;
    }
  }
  return true;
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
  EXPECT_TRUE(MatchesFrozenInterface(parsed.module, frozen))
      << "Frozen host interface differs from parsed WGSL for " << GetParam().artifactTarget;

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
  ASSERT_TRUE(MatchesFrozenInterface(parsed.module, frozen));

  std::vector<ShaderResource> resources(frozen.resources.begin(), frozen.resources.end());
  resources.front().binding += 1;
  CompiledShaderView wrongResource = frozen;
  wrongResource.resources = resources;
  EXPECT_FALSE(MatchesFrozenInterface(parsed.module, wrongResource));

  std::vector<ShaderEntryPoint> entries(frozen.entryPoints.begin(), frozen.entryPoints.end());
  entries.front().stage =
      entries.front().stage == ShaderStage::Compute ? ShaderStage::Vertex : ShaderStage::Compute;
  CompiledShaderView wrongEntry = frozen;
  wrongEntry.entryPoints = entries;
  EXPECT_FALSE(MatchesFrozenInterface(parsed.module, wrongEntry));
}

TEST(WgslProjectionValidation, DetectsStorageFormatAccessAndSampleTypeMismatches) {
  const CompiledShaderView* flood = ShippedShader("flood_artifact");
  ASSERT_NE(flood, nullptr);
  const wgsl::ParseResult parsedFlood = wgsl::Parse(flood->wgsl);
  ASSERT_TRUE(parsedFlood.hasResult());
  ASSERT_TRUE(MatchesFrozenInterface(parsedFlood.module, *flood));

  std::vector<ShaderResource> floodResources(flood->resources.begin(), flood->resources.end());
  const auto output = std::find_if(
      floodResources.begin(), floodResources.end(),
      [](const ShaderResource& resource) { return resource.name.view() == "outputTexture"; });
  ASSERT_NE(output, floodResources.end());
  CompiledShaderView wrongFlood = *flood;
  wrongFlood.resources = floodResources;
  output->storageFormat = TextureFormat::RGBA8Unorm;
  EXPECT_FALSE(MatchesFrozenInterface(parsedFlood.module, wrongFlood));
  output->storageFormat = TextureFormat::RGBA32Float;
  output->type = BindingType::SampledTexture2dFloat;
  EXPECT_FALSE(MatchesFrozenInterface(parsedFlood.module, wrongFlood));

  const CompiledShaderView* matrix = ShippedShader("filter_color_matrix_artifact");
  ASSERT_NE(matrix, nullptr);
  const wgsl::ParseResult parsedMatrix = wgsl::Parse(matrix->wgsl);
  ASSERT_TRUE(parsedMatrix.hasResult());
  ASSERT_TRUE(MatchesFrozenInterface(parsedMatrix.module, *matrix));
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
  EXPECT_FALSE(MatchesFrozenInterface(parsedMatrix.module, wrongMatrix));
}

}  // namespace
}  // namespace donner::gpu::shader
