/// @file
/// Snapshot-unpremultiply compute program tests: the module builds cleanly, all three emitters
/// produce deterministic output, and expose the expected program interfaces.
///
/// This program is the first user of the IR's componentwise comparison, bool-vector reduction,
/// and logical shift right, so its compiler and execution cases also exercise those primitives'
/// forms are pinned in every backend.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/SnapshotUnpremultiply.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

std::string EmitUnpremultiplyWgsl() {
  ShaderResult<IrModule> module = programs::BuildSnapshotUnpremultiplyModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string EmitUnpremultiplyMsl() {
  ShaderResult<IrModule> module = programs::BuildSnapshotUnpremultiplyModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

std::vector<uint32_t> EmitUnpremultiplySpirv() {
  ShaderResult<IrModule> module = programs::BuildSnapshotUnpremultiplyModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return {};
  }
  return GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>());
}

TEST(SnapshotUnpremultiplyProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildSnapshotUnpremultiplyModule(), HasShaderResult());
}

TEST(SnapshotUnpremultiplyProgramTests, EmittersAreDeterministic) {
  EXPECT_THAT(EmitUnpremultiplyWgsl(), testing::Eq(EmitUnpremultiplyWgsl()));
  EXPECT_THAT(EmitUnpremultiplyMsl(), testing::Eq(EmitUnpremultiplyMsl()));
  EXPECT_THAT(EmitUnpremultiplySpirv(), testing::Eq(EmitUnpremultiplySpirv()));
}

TEST(SnapshotUnpremultiplyProgramTests, WgslDeclaresTheComputeSurface) {
  const std::string wgsl = EmitUnpremultiplyWgsl();

  EXPECT_THAT(wgsl, HasSubstr("@compute @workgroup_size(8, 8, 1)\nfn cs_main("));
  EXPECT_THAT(wgsl, HasSubstr("@builtin(global_invocation_id) gid: vec3<u32>"));
  EXPECT_THAT(wgsl, HasSubstr("@group(0) @binding(0) var inputTexture: texture_2d<f32>;"));
  EXPECT_THAT(wgsl,
              HasSubstr("@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba8unorm, "
                        "write>;"));
  // The three primitives this program introduced, in their WGSL spellings.
  EXPECT_THAT(wgsl, HasSubstr("any((gid.xy >= extent))"));
  EXPECT_THAT(wgsl, HasSubstr("(a8 >> 1u)"));
  EXPECT_THAT(wgsl, HasSubstr("textureStore(outputTexture, coords, straight);"));
}

TEST(SnapshotUnpremultiplyProgramTests, MslDeclaresTheKernelSurface) {
  const std::string msl = EmitUnpremultiplyMsl();

  EXPECT_THAT(msl, HasSubstr("kernel void cs_main("));
  EXPECT_THAT(msl, HasSubstr("uint3 gid [[thread_position_in_grid]]"));
  EXPECT_THAT(msl, HasSubstr("texture2d<float> inputTexture [[texture(0)]]"));
  EXPECT_THAT(msl, HasSubstr("texture2d<float, access::write> outputTexture [[texture(1)]]"));
  EXPECT_THAT(msl, HasSubstr("any((gid.xy >= extent))"));
  EXPECT_THAT(msl, HasSubstr("(a8 >> 1u)"));
}

}  // namespace
}  // namespace donner::gpu::shader
