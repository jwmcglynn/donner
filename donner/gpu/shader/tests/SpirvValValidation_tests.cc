/// @file
/// Out-of-process SPIR-V validation: every emitted module must pass
/// `spirv-val --target-env vulkan1.1`. Platform validators run as external verification tools
/// rather than build dependencies, so the test skips cleanly when spirv-val is not installed.
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
#include <string>
#include <vector>

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/programs/ColorMatrix.h"
#include "donner/gpu/shader/programs/Flood.h"
#include "donner/gpu/shader/programs/SnapshotUnpremultiply.h"
#include "donner/gpu/shader/programs/SolidFill.h"
#include "donner/gpu/shader/programs/SubregionClip.h"
#include "donner/gpu/shader/tests/ReductionCoverageModule.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"
#include "donner/gpu/shader/tests/StageIoTestModules.h"

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

/// Locates spirv-val: PATH first, then the common Homebrew and /usr/local install locations.
/// Returns an empty string when unavailable.
std::string FindSpirvVal() {
  const std::array<const char*, 3> candidates = {"spirv-val", "/opt/homebrew/bin/spirv-val",
                                                 "/usr/local/bin/spirv-val"};
  for (const char* candidate : candidates) {
    std::string output;
    if (RunCommand(std::string(candidate) + " --version", &output) == 0) {
      return candidate;
    }
  }
  return "";
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

TEST(SpirvValValidation, EmittedSolidFillPassesVulkan11Validation) {
  const std::string spirvVal = FindSpirvVal();
  if (spirvVal.empty()) {
    GTEST_SKIP() << "spirv-val (SPIRV-Tools) is not installed";
  }
  ExpectValidatesForVulkan11(spirvVal, programs::BuildSolidFillModule(), "solid_fill.spv");
}

TEST(SpirvValValidation, EmittedColorMatrixComputePassesVulkan11Validation) {
  const std::string spirvVal = FindSpirvVal();
  if (spirvVal.empty()) {
    GTEST_SKIP() << "spirv-val (SPIRV-Tools) is not installed";
  }
  ExpectValidatesForVulkan11(spirvVal, programs::BuildColorMatrixModule(), "color_matrix.spv");
}

TEST(SpirvValValidation, EmittedFloodComputePassesVulkan11Validation) {
  const std::string spirvVal = FindSpirvVal();
  if (spirvVal.empty()) {
    GTEST_SKIP() << "spirv-val (SPIRV-Tools) is not installed";
  }
  ExpectValidatesForVulkan11(spirvVal, programs::BuildFloodModule(), "flood.spv");
}

TEST(SpirvValValidation, EmittedSubregionClipComputePassesVulkan11Validation) {
  const std::string spirvVal = FindSpirvVal();
  if (spirvVal.empty()) {
    GTEST_SKIP() << "spirv-val (SPIRV-Tools) is not installed";
  }
  ExpectValidatesForVulkan11(spirvVal, programs::BuildSubregionClipModule(), "subregion_clip.spv");
}

TEST(SpirvValValidation, AStorageBlockHoldingBothMatrixTypesPassesVulkan11Validation) {
  // The MatrixStride decoration is per member, and a validator checks it against the member's
  // own layout: one hardcoded stride would decorate mat2x2f's 8-byte columns as 16 and be
  // rejected here.
  const std::string spirvVal = FindSpirvVal();
  if (spirvVal.empty()) {
    GTEST_SKIP() << "spirv-val (SPIRV-Tools) is not installed";
  }
  ExpectValidatesForVulkan11(spirvVal, BuildMatrixBlockModule(), "matrix_block.spv");
}

TEST(SpirvValValidation, APositionOnlyFragmentEntryPassesVulkan11Validation) {
  // Position is location-less in every emitter, so each decides on its own how such an input
  // reaches the stage. SPIR-V declares it as its own Input variable, decorated FragCoord rather
  // than the vertex stage's Position; the validator is what says so out of process.
  const std::string spirvVal = FindSpirvVal();
  if (spirvVal.empty()) {
    GTEST_SKIP() << "spirv-val (SPIRV-Tools) is not installed";
  }
  ExpectValidatesForVulkan11(spirvVal, BuildPositionOnlyFragmentModule(), "position_only.spv");
}

TEST(SpirvValValidation, EmittedSnapshotUnpremultiplyComputePassesVulkan11Validation) {
  // The first module to emit OpUGreaterThanEqual over a vector, OpAny, and OpShiftRightLogical,
  // so this is where the validator confirms those encodings and their result types.
  const std::string spirvVal = FindSpirvVal();
  if (spirvVal.empty()) {
    GTEST_SKIP() << "spirv-val (SPIRV-Tools) is not installed";
  }
  ExpectValidatesForVulkan11(spirvVal, programs::BuildSnapshotUnpremultiplyModule(),
                             "snapshot_unpremultiply.spv");
}

TEST(SpirvValValidation, EmittedBoolVectorReductionsPassVulkan11Validation) {
  // OpAll reaches no shipping program, so this is the only place the validator confirms its
  // encoding and that its result type is a scalar bool rather than the vector it reduced.
  const std::string spirvVal = FindSpirvVal();
  if (spirvVal.empty()) {
    GTEST_SKIP() << "spirv-val (SPIRV-Tools) is not installed";
  }
  ExpectValidatesForVulkan11(spirvVal, BuildVectorReductionModule(), "vector_reductions.spv");
}

TEST(SpirvValValidation, NegativeControlDetectsAMalformedModule) {
  // Proves the detection mechanism. The emitted solid-fill module is truncated to its header plus
  // a single word, which is not a decodable instruction stream; spirv-val must reject it. Without
  // this, a harness that silently reported success would make the acceptance results above
  // meaningless.
  const std::string spirvVal = FindSpirvVal();
  if (spirvVal.empty()) {
    GTEST_SKIP() << "spirv-val (SPIRV-Tools) is not installed";
  }

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

}  // namespace
}  // namespace donner::gpu::shader
