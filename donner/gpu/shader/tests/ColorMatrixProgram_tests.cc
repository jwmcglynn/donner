/// @file
/// Color-matrix compute program tests: the module builds cleanly, all three emitters produce
/// deterministic output, and each matches its committed golden byte-exactly.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/ColorMatrix.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

/// Packs SPIR-V words little-endian, matching the on-disk golden encoding.
std::string WordsToBytes(const std::vector<uint32_t>& words) {
  std::string bytes;
  bytes.reserve(words.size() * 4);
  for (const uint32_t word : words) {
    bytes.push_back(static_cast<char>(word & 0xFF));
    bytes.push_back(static_cast<char>((word >> 8) & 0xFF));
    bytes.push_back(static_cast<char>((word >> 16) & 0xFF));
    bytes.push_back(static_cast<char>((word >> 24) & 0xFF));
  }
  return bytes;
}

std::string EmitColorMatrixWgsl() {
  ShaderResult<IrModule> module = programs::BuildColorMatrixModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string EmitColorMatrixMsl() {
  ShaderResult<IrModule> module = programs::BuildColorMatrixModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

std::string EmitColorMatrixSpirvBytes() {
  ShaderResult<IrModule> module = programs::BuildColorMatrixModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return WordsToBytes(GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>()));
}

std::string ReadGolden(const std::string& name) {
  const std::string path =
      donner::Runfiles::instance().Rlocation("donner/gpu/shader/tests/testdata/" + name);
  std::ifstream stream(path, std::ios::binary);
  EXPECT_TRUE(stream.good()) << "Failed to open golden file: " << path;
  std::ostringstream contents;
  contents << stream.rdbuf();
  return contents.str();
}

/// Rewrites a golden when its update environment variable names a repository root, and returns
/// true so the caller can skip the comparison.
/// @param environmentVariable Name of the update variable, e.g. `UPDATE_WGSL_GOLDEN`.
/// @param name Golden file name under `donner/gpu/shader/tests/testdata`.
/// @param contents Bytes to write.
bool MaybeUpdateGolden(const char* environmentVariable, const std::string& name,
                       const std::string& contents) {
  const char* updateRoot = std::getenv(environmentVariable);
  if (updateRoot == nullptr) {
    return false;
  }
  const std::string outPath = std::string(updateRoot) + "/donner/gpu/shader/tests/testdata/" + name;
  std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
  EXPECT_TRUE(out.good()) << "Failed to open " << outPath << " for writing";
  out << contents;
  return true;
}

TEST(ColorMatrixProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildColorMatrixModule(), HasShaderResult());
}

TEST(ColorMatrixProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitColorMatrixWgsl(), testing::Eq(EmitColorMatrixWgsl()));
  EXPECT_THAT(EmitColorMatrixMsl(), testing::Eq(EmitColorMatrixMsl()));
  EXPECT_THAT(EmitColorMatrixSpirvBytes(), testing::Eq(EmitColorMatrixSpirvBytes()));
}

TEST(ColorMatrixProgramTests, WgslDeclaresTheComputeSurface) {
  const std::string wgsl = EmitColorMatrixWgsl();

  EXPECT_THAT(wgsl, HasSubstr("@compute @workgroup_size(8, 8, 1)\nfn cs_main("));
  EXPECT_THAT(wgsl, HasSubstr("@builtin(global_invocation_id) gid: vec3<u32>"));
  EXPECT_THAT(wgsl,
              HasSubstr("@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba8unorm, "
                        "write>;"));
  EXPECT_THAT(wgsl, HasSubstr("@group(0) @binding(0) var inputTexture: texture_2d<f32>;"));
  EXPECT_THAT(wgsl, HasSubstr("@group(0) @binding(2) var<uniform> params: ColorMatrixParams;"));
  EXPECT_THAT(wgsl, HasSubstr("@group(0) @binding(3) var<storage, read> bias: array<vec4<f32>>;"));
  EXPECT_THAT(wgsl, HasSubstr("textureStore(outputTexture, coords, result);"));
  // A compute entry point returns nothing, so no generated output struct may appear.
  EXPECT_THAT(wgsl, testing::Not(HasSubstr("cs_main_Output")));
}

TEST(ColorMatrixProgramTests, MslDeclaresTheKernelSurface) {
  const std::string msl = EmitColorMatrixMsl();

  EXPECT_THAT(msl, HasSubstr("kernel void cs_main("));
  EXPECT_THAT(msl, HasSubstr("uint3 gid [[thread_position_in_grid]]"));
  EXPECT_THAT(msl, HasSubstr("texture2d<float, access::write> outputTexture [[texture(1)]]"));
  EXPECT_THAT(msl, HasSubstr("texture2d<float> inputTexture [[texture(0)]]"));
  EXPECT_THAT(msl, HasSubstr("constant ColorMatrixParams& params [[buffer(3)]]"));
  EXPECT_THAT(msl, HasSubstr("device const float4* bias [[buffer(4)]]"));
  EXPECT_THAT(msl, HasSubstr("outputTexture.write(result, uint2(coords));"));
  // A kernel takes its builtins directly, so no stage-in struct may appear.
  EXPECT_THAT(msl, testing::Not(HasSubstr("stage_in")));
}

TEST(ColorMatrixProgramTests, WgslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_WGSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string wgsl = EmitColorMatrixWgsl();
  if (MaybeUpdateGolden("UPDATE_WGSL_GOLDEN", "color_matrix.wgsl", wgsl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(wgsl, testing::Eq(ReadGolden("color_matrix.wgsl")));
}

TEST(ColorMatrixProgramTests, MslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_MSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string msl = EmitColorMatrixMsl();
  if (MaybeUpdateGolden("UPDATE_MSL_GOLDEN", "color_matrix.msl", msl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(msl, testing::Eq(ReadGolden("color_matrix.msl")));
}

TEST(ColorMatrixProgramTests, SpirvMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_SPIRV_GOLDEN=/path/to/repo rewrites the golden.
  const std::string bytes = EmitColorMatrixSpirvBytes();
  if (MaybeUpdateGolden("UPDATE_SPIRV_GOLDEN", "color_matrix.spv", bytes)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(bytes, testing::Eq(ReadGolden("color_matrix.spv")));
}

}  // namespace
}  // namespace donner::gpu::shader
