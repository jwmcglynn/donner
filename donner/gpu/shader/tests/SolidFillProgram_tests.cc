/// @file
/// Solid-fill program tests: the module builds cleanly, emits deterministically, and matches the
/// committed WGSL golden byte-exactly.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <string>

#include "donner/base/tests/Runfiles.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/SolidFill.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

std::string EmitSolidFill() {
  ShaderResult<IrModule> module = programs::BuildSolidFillModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string ReadGoldenFile() {
  const std::string path =
      donner::Runfiles::instance().Rlocation("donner/gpu/shader/tests/testdata/solid_fill.wgsl");
  std::ifstream stream(path, std::ios::binary);
  EXPECT_TRUE(stream.good()) << "Failed to open golden file: " << path;
  std::ostringstream contents;
  contents << stream.rdbuf();
  return contents.str();
}

TEST(SolidFillProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildSolidFillModule(), HasShaderResult());
}

TEST(SolidFillProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitSolidFill(), testing::Eq(EmitSolidFill()));
}

TEST(SolidFillProgramTests, ContainsSlugFillSurface) {
  const std::string wgsl = EmitSolidFill();

  // Entry points and every binding index of the slug fill pipeline.
  EXPECT_THAT(wgsl, HasSubstr("@vertex\nfn vs_main("));
  EXPECT_THAT(wgsl, HasSubstr("@fragment\nfn fs_main("));
  for (int binding = 0; binding <= 11; ++binding) {
    EXPECT_THAT(wgsl, HasSubstr(std::format("@binding({}) ", binding)));
  }
  EXPECT_THAT(wgsl, HasSubstr("const kNoBand: u32 = 4294967295u;"));
  EXPECT_THAT(wgsl, HasSubstr("discard;"));
}

TEST(SolidFillProgramTests, MatchesCommittedGoldenByteExactly) {
  // The golden is regenerated deliberately: run with
  // UPDATE_WGSL_GOLDEN=/path/to/repo to rewrite it, then review the diff.
  const std::string wgsl = EmitSolidFill();

  if (const char* updateRoot = std::getenv("UPDATE_WGSL_GOLDEN")) {
    const std::string outPath =
        std::string(updateRoot) + "/donner/gpu/shader/tests/testdata/solid_fill.wgsl";
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "Failed to open " << outPath << " for writing";
    out << wgsl;
    GTEST_SKIP() << "Golden updated at " << outPath;
  }

  EXPECT_THAT(wgsl, testing::Eq(ReadGoldenFile()));
}

}  // namespace
}  // namespace donner::gpu::shader
