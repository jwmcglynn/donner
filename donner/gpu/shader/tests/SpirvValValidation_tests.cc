/// @file
/// Out-of-process SPIR-V validation: every emitted module must pass
/// `spirv-val --target-env vulkan1.1`. The validator is built from source by Bazel and reached
/// through runfiles, so every machine runs the same one and a machine without it cannot exist.
///
/// A negative control proves the detection mechanism: a deliberately malformed module must be
/// rejected, so an acceptance result here means the validator actually inspected the words rather
/// than the harness silently reporting success.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/programs/ColorMatrix.h"
#include "donner/gpu/shader/programs/SolidFill.h"
#include "donner/gpu/shader/tests/CompiledCheckerboard.h"
#include "donner/gpu/shader/tests/CompiledColorSpaceConvert.h"
#include "donner/gpu/shader/tests/CompiledComponentTransfer.h"
#include "donner/gpu/shader/tests/CompiledComposite.h"
#include "donner/gpu/shader/tests/CompiledConvolve.h"
#include "donner/gpu/shader/tests/CompiledDiffuseLighting.h"
#include "donner/gpu/shader/tests/CompiledDisplacementMap.h"
#include "donner/gpu/shader/tests/CompiledDropShadow.h"
#include "donner/gpu/shader/tests/CompiledFilterBlend.h"
#include "donner/gpu/shader/tests/CompiledFilterColorMatrix.h"
#include "donner/gpu/shader/tests/CompiledFilterImage.h"
#include "donner/gpu/shader/tests/CompiledFilterResolve.h"
#include "donner/gpu/shader/tests/CompiledFlood.h"
#include "donner/gpu/shader/tests/CompiledGaussian.h"
#include "donner/gpu/shader/tests/CompiledImageBlit.h"
#include "donner/gpu/shader/tests/CompiledMerge.h"
#include "donner/gpu/shader/tests/CompiledMorphology.h"
#include "donner/gpu/shader/tests/CompiledOffset.h"
#include "donner/gpu/shader/tests/CompiledSlugFill.h"
#include "donner/gpu/shader/tests/CompiledSlugGradient.h"
#include "donner/gpu/shader/tests/CompiledSlugMask.h"
#include "donner/gpu/shader/tests/CompiledSnapshotUnpremultiply.h"
#include "donner/gpu/shader/tests/CompiledSpecularLighting.h"
#include "donner/gpu/shader/tests/CompiledSubregionClip.h"
#include "donner/gpu/shader/tests/CompiledTile.h"
#include "donner/gpu/shader/tests/CompiledTurbulence.h"
#include "donner/gpu/shader/tests/FloatStorageModule.h"
#include "donner/gpu/shader/tests/MathPrimitiveCoverageModule.h"
#include "donner/gpu/shader/tests/ReductionCoverageModule.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"
#include "donner/gpu/shader/tests/StageIoTestModules.h"
#include "donner/gpu/shader/wgsl/tests/GraphicsArtifact.h"

using testing::HasSubstr;
using testing::Not;

namespace donner::gpu::shader {
namespace {

/// Runs \p command, capturing combined stdout+stderr; returns the exit status or -1.
int RunCommand(const std::string& command, std::string* output) {
  output->clear();
  FILE* pipe = popen((command + " 2>&1").c_str(), "r");
  if (pipe == nullptr) {
    return -1;
  }
  std::array<char, 4096> buffer;
  size_t bytesRead = 0;
  while ((bytesRead = fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
    output->append(buffer.data(), bytesRead);
  }
  return pclose(pipe);
}

/// The Bazel-built validator, from this test's runfiles. Never a host install: which validator ran
/// would otherwise vary by machine, and a machine without one validated nothing.
std::string SpirvVal() {
  return Runfiles::instance().RlocationExternal("spirv_tools", "spirv-val");
}

/// Writes \p words under TEST_TMPDIR as \p fileName and returns spirv-val's combined output,
/// setting \p status to its exit code. Used by the negative control, which needs the failure
/// itself rather than an assertion on it.
/// @param spirvVal Path to the spirv-val executable.
/// @param words SPIR-V word stream to validate.
/// @param fileName File name to write the words to.
/// @param status Set to spirv-val's exit status.
std::string ValidateWordsForStatus(const std::string& spirvVal, const std::vector<uint32_t>& words,
                                   const std::string& fileName, int* status) {
  const char* testTmpdir = std::getenv("TEST_TMPDIR");
  if (testTmpdir == nullptr) {
    *status = -1;
    return "TEST_TMPDIR is unset";
  }
  const std::string modulePath = std::string(testTmpdir) + "/" + fileName;
  {
    std::ofstream out(modulePath, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
      *status = -1;
      return "Failed to write " + modulePath;
    }
    for (const uint32_t word : words) {
      const std::array<char, 4> bytes = {
          static_cast<char>(word & 0xFF), static_cast<char>((word >> 8) & 0xFF),
          static_cast<char>((word >> 16) & 0xFF), static_cast<char>((word >> 24) & 0xFF)};
      out.write(bytes.data(), bytes.size());
    }
  }

  std::string output;
  *status = RunCommand(spirvVal + " --target-env vulkan1.1 \"" + modulePath + "\"", &output);
  return output;
}

/// Validates frozen SPIR-V words through the same external validator path.
void ExpectWordsValidateForVulkan11(const std::string& spirvVal, std::span<const uint32_t> words,
                                    const std::string& fileName) {
  std::vector<uint32_t> copied(words.begin(), words.end());
  int validationStatus = -1;
  const std::string validationOutput =
      ValidateWordsForStatus(spirvVal, copied, fileName, &validationStatus);
  EXPECT_EQ(validationStatus, 0) << "spirv-val rejected " << fileName << ":\n" << validationOutput;
  EXPECT_THAT(validationOutput, Not(HasSubstr("error"))) << validationOutput;
}

/// Emits \p module, writes it under TEST_TMPDIR as \p fileName, and asserts spirv-val accepts it.
/// @param spirvVal Path to the spirv-val executable.
/// @param module Built IR module to emit and validate.
/// @param fileName File name to write the emitted words to.
void ExpectValidatesForVulkan11(const std::string& spirvVal, ShaderResult<IrModule>&& module,
                                const std::string& fileName) {
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::vector<uint32_t>> spirv = EmitSpirv(module.result());
  ASSERT_FALSE(spirv.hasError()) << "EmitSpirv failed: " << spirv.error();

  const char* testTmpdir = std::getenv("TEST_TMPDIR");
  ASSERT_NE(testTmpdir, nullptr);
  const std::string modulePath = std::string(testTmpdir) + "/" + fileName;
  {
    std::ofstream out(modulePath, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "Failed to write " << modulePath;
    for (const uint32_t word : spirv.result()) {
      const std::array<char, 4> bytes = {
          static_cast<char>(word & 0xFF), static_cast<char>((word >> 8) & 0xFF),
          static_cast<char>((word >> 16) & 0xFF), static_cast<char>((word >> 24) & 0xFF)};
      out.write(bytes.data(), bytes.size());
    }
  }

  std::string validationOutput;
  const int validationStatus =
      RunCommand(spirvVal + " --target-env vulkan1.1 \"" + modulePath + "\"", &validationOutput);

  EXPECT_EQ(validationStatus, 0) << "spirv-val rejected " << fileName << ":\n" << validationOutput;
  EXPECT_THAT(validationOutput, Not(HasSubstr("error"))) << validationOutput;
  if (!validationOutput.empty()) {
    // Surface warnings in the test log even when validation succeeds.
    std::fprintf(stderr, "spirv-val output for %s:\n%s\n", fileName.c_str(),
                 validationOutput.c_str());
  }
}

/// Builds a module whose read-only storage block holds both matrix types, so the per-member
/// MatrixStride decoration is exercised for each. mat2x2f is function-local in the shipped
/// programs, which leaves the buffer-layout path for it uncovered; this is the module that covers
/// it.
ShaderResult<IrModule> BuildMatrixBlockModule() {
  ModuleBuilder builder;

  ShaderResult<IrType> axesStruct =
      IrType::Struct("Axes", {{"axes", IrType::Mat2x2f()}, {"mvp", IrType::Mat4x4f()}});
  if (axesStruct.hasError()) {
    return std::move(axesStruct).error();
  }
  ShaderResult<IrType> axesArray = IrType::RuntimeArray(axesStruct.result());
  if (axesArray.hasError()) {
    return std::move(axesArray).error();
  }
  if (ShaderStatus status = builder.addReadOnlyStorageBuffer(0, 0, "shapes", axesArray.result());
      status.hasError()) {
    return std::move(status).error();
  }

  ShaderResult<FunctionBuilder> entry = builder.createVertexEntryPoint(
      "vs_test", {IrParam{"vertex_index", IrType::U32(), std::nullopt, BuiltinInput::VertexIndex}},
      {IrOutputMember{"clip_pos", IrType::Vec4f(), std::nullopt, BuiltinOutput::Position}});
  if (entry.hasError()) {
    return std::move(entry).error();
  }
  FunctionBuilder fn = std::move(entry).result();

  ShaderResult<IrExpr> shape = Index(GetShaderResultOrFail(fn.ref("shapes"), LiteralF32(0.0f)),
                                     GetShaderResultOrFail(fn.ref("vertex_index"), LiteralU32(0u)));
  const IrExpr axes = GetShaderResultOrFail(Member(shape.result(), "axes"), LiteralF32(0.0f));
  const IrExpr column = GetShaderResultOrFail(Index(axes, LiteralU32(0u)), LiteralF32(0.0f));
  const IrExpr mapped = GetShaderResultOrFail(Mul(axes, column), LiteralF32(0.0f));
  const IrExpr mvp = GetShaderResultOrFail(Member(shape.result(), "mvp"), LiteralF32(0.0f));
  const IrExpr clip = GetShaderResultOrFail(
      Mul(mvp, GetShaderResultOrFail(
                   ConstructVector(IrType::Vec4f(), {mapped, LiteralF32(0.0f), LiteralF32(1.0f)}),
                   LiteralF32(0.0f))),
      LiteralF32(0.0f));
  if (ShaderStatus status = fn.returnOutputs({clip}); status.hasError()) {
    return std::move(status).error();
  }
  if (ShaderStatus status = fn.finish(); status.hasError()) {
    return std::move(status).error();
  }
  return builder.build();
}

TEST(SpirvValValidation, EmittedCheckerboardPassesVulkan11Validation) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::CheckerboardAllProjections().spirv,
                                 "checkerboard.spv");
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::CheckerboardMutatedAllProjections().spirv,
                                 "checkerboard_mutated.spv");
}

TEST(SpirvValValidation, FinalFilterResolvePassesVulkan11Validation) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::FilterResolveAllProjections().spirv,
                                 "filter_resolve.spv");
}
TEST(SpirvValValidation, EmittedCompositePassesVulkan11Validation) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::CompositeAllProjections().spirv,
                                 "composite.spv");
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::CompositeMutatedAllProjections().spirv,
                                 "composite_mutated.spv");
}

TEST(SpirvValValidation, CompiledGraphicsEntriesPassVulkan11Validation) {
  ExpectWordsValidateForVulkan11(SpirvVal(), wgsl::tests::GraphicsShader().spirv,
                                 "compiled_graphics.spv");
  ExpectWordsValidateForVulkan11(SpirvVal(), wgsl::tests::StorageArrayShader().spirv,
                                 "storage_arrays.spv");
  ExpectWordsValidateForVulkan11(SpirvVal(), wgsl::tests::ControlShader().spirv, "numeric_control");
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::SlugMaskAllProjections().spirv, "slug_mask");
  ExpectWordsValidateForVulkan11(SpirvVal(), wgsl::tests::MatrixShader().spirv,
                                 "compiled_matrices.spv");
  ExpectWordsValidateForVulkan11(SpirvVal(), wgsl::tests::MatrixOperationsShader().spirv,
                                 "matrix_operations.spv");
}

TEST(SpirvValValidation, EmittedConvolveMatrixPassesVulkan11Validation) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::ConvolveMatrixAllProjections().spirv,
                                 "convolve_matrix.spv");
}

TEST(SpirvValValidation, EmittedMergePassesVulkan11Validation) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::MergeAllProjections().spirv, "merge.spv");
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::MergeMutatedAllProjections().spirv,
                                 "merge_mutated.spv");
}

TEST(SpirvValValidation, EmittedSolidFillPassesVulkan11Validation) {
  const std::string spirvVal = SpirvVal();
  ExpectValidatesForVulkan11(spirvVal, programs::BuildSolidFillModule(), "solid_fill.spv");
}

TEST(SpirvValValidation, EmittedColorMatrixComputePassesVulkan11Validation) {
  const std::string spirvVal = SpirvVal();
  ExpectValidatesForVulkan11(spirvVal, programs::BuildColorMatrixModule(), "color_matrix.spv");
}

TEST(SpirvValValidation, EmittedFloodComputePassesVulkan11Validation) {
  const std::string spirvVal = SpirvVal();
  ExpectWordsValidateForVulkan11(spirvVal, tests::FloodAllProjections().spirv, "flood.spv");
  ExpectWordsValidateForVulkan11(spirvVal, tests::FloodMutatedAllProjections().spirv,
                                 "flood_mutated.spv");
}

TEST(SpirvValValidation, EmittedSubregionClipComputePassesVulkan11Validation) {
  const std::string spirvVal = SpirvVal();
  ExpectWordsValidateForVulkan11(spirvVal, tests::SubregionClipAllProjections().spirv,
                                 "subregion_clip.spv");
  ExpectWordsValidateForVulkan11(spirvVal, tests::SubregionClipMutatedAllProjections().spirv,
                                 "subregion_clip_mutated.spv");
}

TEST(SpirvValValidation, EmittedFilterColorMatrixPassesVulkan11Validation) {
  const std::string spirvVal = SpirvVal();
  ExpectWordsValidateForVulkan11(spirvVal, tests::FilterColorMatrixAllProjections().spirv,
                                 "filter_color_matrix.spv");
  ExpectWordsValidateForVulkan11(spirvVal, tests::FilterColorMatrixMutatedAllProjections().spirv,
                                 "filter_color_matrix_mutated.spv");
}

TEST(SpirvValValidation, EmittedFilterImagePassesVulkan11Validation) {
  const std::string spirvVal = SpirvVal();
  ExpectWordsValidateForVulkan11(spirvVal, tests::FilterImageAllProjections().spirv,
                                 "filter_image.spv");
  ExpectWordsValidateForVulkan11(spirvVal, tests::FilterImageMutatedAllProjections().spirv,
                                 "filter_image_mutated.spv");
}

TEST(SpirvValValidation, EmittedOffsetComputePassesVulkan11Validation) {
  // The first compute program to emit an OpFunctionCall, and the first shipping program to reach
  // FSign and Floor, so this is where the validator confirms those encodings inside a real one.
  const std::string spirvVal = SpirvVal();
  ExpectWordsValidateForVulkan11(spirvVal, tests::OffsetAllProjections().spirv, "offset.spv");
  ExpectWordsValidateForVulkan11(spirvVal, tests::FloorAllProjections().spirv, "wgsl_floor.spv");
  ExpectWordsValidateForVulkan11(spirvVal, tests::SignAllProjections().spirv, "wgsl_sign.spv");
}

TEST(SpirvValValidation, EmittedTileComputePassesVulkan11Validation) {
  // The first compute program to emit an OpFunctionCall, and the first shipping program to reach
  // FSign and Floor, so this is where the validator confirms those encodings inside a real one.
  const std::string spirvVal = SpirvVal();
  ExpectWordsValidateForVulkan11(spirvVal, tests::TileAllProjections().spirv, "tile.spv");
  ExpectWordsValidateForVulkan11(spirvVal, tests::TileMutatedAllProjections().spirv,
                                 "tile_mutated.spv");
}

TEST(SpirvValValidation, EmittedComponentTransferComputePassesVulkan11Validation) {
  // The first compute program to emit an OpFunctionCall, and the first shipping program to reach
  // FSign and Floor, so this is where the validator confirms those encodings inside a real one.
  const std::string spirvVal = SpirvVal();
  ExpectWordsValidateForVulkan11(spirvVal, tests::ComponentTransferAllProjections().spirv,
                                 "component_transfer.spv");
  ExpectWordsValidateForVulkan11(spirvVal, tests::ComponentTransferMutatedAllProjections().spirv,
                                 "component_transfer_mutated.spv");
}

TEST(SpirvValValidation, EmittedTurbulenceComputePassesVulkan11Validation) {
  const std::string spirvVal = SpirvVal();
  ExpectWordsValidateForVulkan11(spirvVal, tests::TurbulenceAllProjections().spirv,
                                 "turbulence.spv");
  ExpectWordsValidateForVulkan11(spirvVal, tests::EightArgumentCallAllProjections().spirv,
                                 "eight_arguments.spv");
}

TEST(SpirvValValidation, EmittedDropShadowComputePassesVulkan11Validation) {
  // The first compute program to emit an OpFunctionCall, and the first shipping program to reach
  // FSign and Floor, so this is where the validator confirms those encodings inside a real one.
  const std::string spirvVal = SpirvVal();
  ExpectWordsValidateForVulkan11(spirvVal, tests::DropShadowAllProjections().spirv,
                                 "drop_shadow.spv");
  ExpectWordsValidateForVulkan11(spirvVal, tests::DropShadowMutatedAllProjections().spirv,
                                 "drop_shadow_mutated.spv");
}

TEST(SpirvValValidation, EmittedDisplacementMapComputePassesVulkan11Validation) {
  const std::string spirvVal = SpirvVal();
  ExpectWordsValidateForVulkan11(spirvVal, tests::DisplacementMapAllProjections().spirv,
                                 "displacement_map.spv");
  ExpectWordsValidateForVulkan11(spirvVal, tests::DisplacementMapMutatedAllProjections().spirv,
                                 "displacement_map_mutated.spv");
}

TEST(SpirvValValidation, EmittedGaussianBlurComputePassesVulkan11Validation) {
  const std::string spirvVal = SpirvVal();
  ExpectWordsValidateForVulkan11(spirvVal, tests::GaussianBlurAllProjections().spirv,
                                 "gaussian_blur.spv");
}

TEST(SpirvValValidation, EmittedMorphologyComputePassesVulkan11Validation) {
  // The first compute program to emit an OpFunctionCall, and the first shipping program to reach
  // FSign and Floor, so this is where the validator confirms those encodings inside a real one.
  const std::string spirvVal = SpirvVal();
  ExpectWordsValidateForVulkan11(spirvVal, tests::MorphologyAllProjections().spirv,
                                 "morphology.spv");
  ExpectWordsValidateForVulkan11(spirvVal, tests::MorphologyMutatedAllProjections().spirv,
                                 "morphology_mutated.spv");
}

TEST(SpirvValValidation, EmittedLightingComputesPassVulkan11Validation) {
  const std::string spirvVal = SpirvVal();
  ExpectWordsValidateForVulkan11(spirvVal, tests::DiffuseLightingAllProjections().spirv,
                                 "diffuse_lighting.spv");
  ExpectWordsValidateForVulkan11(spirvVal, tests::DiffuseLightingMutatedAllProjections().spirv,
                                 "diffuse_lighting_mutated.spv");
  ExpectWordsValidateForVulkan11(spirvVal, tests::LightingMathAllProjections().spirv,
                                 "lighting_vector_math.spv");
  ExpectWordsValidateForVulkan11(spirvVal, tests::SpecularLightingAllProjections().spirv,
                                 "specular_lighting.spv");
}

TEST(SpirvValValidation, EmittedColorSpaceConvertPassesVulkan11Validation) {
  // The first shipping program to reach the Pow extended instruction, and one whose functions
  // return out of a structured branch, so the validator is what confirms the merge blocks the
  // emitter writes around those returns are well formed.
  const std::string spirvVal = SpirvVal();
  ExpectWordsValidateForVulkan11(spirvVal, tests::ColorSpaceConvertAllProjections().spirv,
                                 "color_space_convert.spv");
  ExpectWordsValidateForVulkan11(spirvVal, tests::ColorSpaceConvertMutatedAllProjections().spirv,
                                 "color_space_convert_mutated.spv");
}

TEST(SpirvValValidation, AStorageBlockHoldingBothMatrixTypesPassesVulkan11Validation) {
  // The MatrixStride decoration is per member, and a validator checks it against the member's
  // own layout: one hardcoded stride would decorate mat2x2f's 8-byte columns as 16 and be
  // rejected here.
  const std::string spirvVal = SpirvVal();
  ExpectValidatesForVulkan11(spirvVal, BuildMatrixBlockModule(), "matrix_block.spv");
}

TEST(SpirvValValidation, APositionOnlyFragmentEntryPassesVulkan11Validation) {
  // Position is location-less in every emitter, so each decides on its own how such an input
  // reaches the stage. SPIR-V declares it as its own Input variable, decorated FragCoord rather
  // than the vertex stage's Position; the validator is what says so out of process.
  const std::string spirvVal = SpirvVal();
  ExpectValidatesForVulkan11(spirvVal, BuildPositionOnlyFragmentModule(), "position_only.spv");
}

TEST(SpirvValValidation, EmittedSnapshotUnpremultiplyComputePassesVulkan11Validation) {
  // The first module to emit OpUGreaterThanEqual over a vector, OpAny, and OpShiftRightLogical,
  // so this is where the validator confirms those encodings and their result types.
  const std::string spirvVal = SpirvVal();
  ExpectWordsValidateForVulkan11(spirvVal, tests::SnapshotUnpremultiplyAllProjections().spirv,
                                 "snapshot_unpremultiply.spv");
  ExpectWordsValidateForVulkan11(spirvVal,
                                 tests::SnapshotUnpremultiplyMutatedAllProjections().spirv,
                                 "snapshot_unpremultiply_mutated.spv");
}

TEST(SpirvValValidation, EmittedBoolVectorReductionsPassVulkan11Validation) {
  // OpAll reaches no shipping program, so this is the only place the validator confirms its
  // encoding and that its result type is a scalar bool rather than the vector it reduced.
  const std::string spirvVal = SpirvVal();
  ExpectValidatesForVulkan11(spirvVal, BuildVectorReductionModule(), "vector_reductions.spv");
}

TEST(SpirvValValidation, EmittedMathPrimitivesPassVulkan11Validation) {
  // The validator is what confirms FSign, Floor, and Pow were given operand counts and result
  // types the extended instruction set actually declares for them.
  const std::string spirvVal = SpirvVal();
  ExpectValidatesForVulkan11(spirvVal, BuildMathPrimitiveModule(), "math_primitives.spv");
}

TEST(SpirvValValidation, FloatStorageTexturePassesVulkan11Validation) {
  ExpectValidatesForVulkan11(SpirvVal(), BuildFloatStorageModule(), "float_storage.spv");
}

TEST(SpirvValValidation, NegativeControlDetectsAMalformedModule) {
  // Proves the detection mechanism. The emitted solid-fill module is truncated to its header plus
  // a single word, which is not a decodable instruction stream; spirv-val must reject it. Without
  // this, a harness that silently reported success would make the acceptance results above
  // meaningless.
  const std::string spirvVal = SpirvVal();

  ShaderResult<IrModule> module = programs::BuildSolidFillModule();
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::vector<uint32_t>> spirv = EmitSpirv(module.result());
  ASSERT_FALSE(spirv.hasError()) << "EmitSpirv failed: " << spirv.error();
  ASSERT_GT(spirv.result().size(), 6u);

  std::vector<uint32_t> malformed(spirv.result().begin(), spirv.result().begin() + 6);

  int status = 0;
  const std::string output = ValidateWordsForStatus(spirvVal, malformed, "malformed.spv", &status);

  EXPECT_NE(status, 0) << "spirv-val accepted a truncated module:\n" << output;
  EXPECT_THAT(output, HasSubstr("error")) << output;
}

TEST(SpirvValValidation, ImageBlit) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::ImageBlitAllProjections().spirv,
                                 "ImageBlit.spv");
}
TEST(SpirvValValidation, ImageBlitMutated) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::ImageBlitMutatedAllProjections().spirv,
                                 "ImageBlitMutated.spv");
}
TEST(SpirvValValidation, ImageBlitExplicitLevel) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::ImageBlitExplicitLevelAllProjections().spirv,
                                 "ImageBlitExplicitLevel.spv");
}
TEST(SpirvValValidation, ArraySwitch) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::ArraySwitchAllProjections().spirv,
                                 "ArraySwitch.spv");
}
TEST(SpirvValValidation, LoopSwitch) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::LoopSwitchAllProjections().spirv,
                                 "LoopSwitch.spv");
}
TEST(SpirvValValidation, StructConstruction) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::StructConstructionAllProjections().spirv,
                                 "StructConstruction.spv");
}

TEST(SpirvValValidation, VectorMix) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::VectorMixAllProjections().spirv,
                                 "VectorMix.spv");
}

TEST(SpirvValValidation, SlugGradient) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::SlugGradientAllProjections().spirv,
                                 "slug_gradient.spv");
}
TEST(SpirvValValidation, SlugGradientMutated) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::SlugGradientMutatedAllProjections().spirv,
                                 "slug_gradientMutated.spv");
}
TEST(SpirvValValidation, FilterBlend) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::FilterBlendAllProjections().spirv,
                                 "filter_blend.spv");
}
TEST(SpirvValValidation, FilterBlendMutated) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::FilterBlendMutatedAllProjections().spirv,
                                 "filter_blendMutated.spv");
}

TEST(SpirvValValidation, SlugFill) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::SlugFillAllProjections().spirv,
                                 "slug_fill.spv");
}
TEST(SpirvValValidation, SlugFillMutated) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::SlugFillMutatedAllProjections().spirv,
                                 "slug_fillMutated.spv");
}

TEST(SpirvValValidation, SlugFlatInterface) {
  ExpectWordsValidateForVulkan11(SpirvVal(), tests::SlugFlatInterfaceAllProjections().spirv,
                                 "slug_flat.spv");
}

}  // namespace
}  // namespace donner::gpu::shader
