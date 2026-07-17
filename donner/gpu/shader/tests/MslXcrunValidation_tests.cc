/// @file
/// Out-of-process MSL validation: the emitted solid-fill MSL must compile cleanly with the
/// platform Metal compiler (`xcrun -sdk macosx metal`). Per design 0053, platform compilers are
/// out-of-process verification tools, not build dependencies.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/programs/SolidFill.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

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

TEST(MslXcrunValidation, EmittedSolidFillCompilesWithMetalCompiler) {
  // Locate the Metal compiler; skip with a clear reason when the toolchain is unavailable.
  // Note: recent Xcode versions ship the offline Metal compiler as a downloadable component
  // (xcodebuild -downloadComponent MetalToolchain); `xcrun --find metal` can succeed while the
  // tool itself is absent, so probe-compile a trivial kernel to detect that case.
  std::string findOutput;
  const int findStatus = RunCommand("xcrun -sdk macosx --find metal", &findOutput);
  if (findStatus != 0) {
    GTEST_SKIP() << "xcrun / Metal compiler unavailable: " << findOutput;
  }
  {
    const char* probeTmpdir = std::getenv("TEST_TMPDIR");
    ASSERT_NE(probeTmpdir, nullptr);
    const std::string probePath = std::string(probeTmpdir) + "/probe.metal";
    std::ofstream probe(probePath, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(probe.good());
    probe << "kernel void donnerProbe() {}\n";
    probe.close();
    std::string probeOutput;
    const int probeStatus = RunCommand("xcrun -sdk macosx metal -std=metal3.0 -c \"" + probePath +
                                           "\" -o \"" + probePath + ".air\"",
                                       &probeOutput);
    if (probeStatus != 0 || probeOutput.find("missing Metal Toolchain") != std::string::npos) {
      GTEST_SKIP() << "Offline Metal compiler unavailable (the runtime Metal framework "
                      "compiler used by the vertical slice test is unaffected): "
                   << probeOutput;
    }
  }

  ShaderResult<IrModule> module = programs::BuildSolidFillModule();
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::string> msl = EmitMsl(module.result());
  ASSERT_FALSE(msl.hasError()) << "EmitMsl failed: " << msl.error();

  const char* testTmpdir = std::getenv("TEST_TMPDIR");
  ASSERT_NE(testTmpdir, nullptr);
  const std::string sourcePath = std::string(testTmpdir) + "/solid_fill.metal";
  const std::string outputPath = std::string(testTmpdir) + "/solid_fill.air";
  {
    std::ofstream out(sourcePath, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "Failed to write " << sourcePath;
    out << msl.result();
  }

  std::string compileOutput;
  const int compileStatus = RunCommand(
      "xcrun -sdk macosx metal -std=metal3.0 -c \"" + sourcePath + "\" -o \"" + outputPath + "\"",
      &compileOutput);

  EXPECT_EQ(compileStatus, 0) << "Metal compiler rejected the emitted MSL:\n" << compileOutput;
  EXPECT_THAT(compileOutput, Not(HasSubstr("error:"))) << compileOutput;
  if (!compileOutput.empty()) {
    // Surface warnings in the test log even when compilation succeeds.
    std::fprintf(stderr, "metal compiler output:\n%s\n", compileOutput.c_str());
  }
}

}  // namespace
}  // namespace donner::gpu::shader
