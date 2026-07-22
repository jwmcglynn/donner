/// @file
/// Out-of-process SPIR-V validation: the emitted solid-fill module must pass
/// `spirv-val --target-env vulkan1.1`. Per design 0053, platform validators are out-of-process
/// verification tools, not build dependencies; the test skips cleanly when spirv-val is not
/// installed.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "donner/gpu/shader/SpirvEmitter.h"
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

TEST(SpirvValValidation, EmittedSolidFillPassesVulkan11Validation) {
  const std::string spirvVal = FindSpirvVal();
  if (spirvVal.empty()) {
    GTEST_SKIP() << "spirv-val (SPIRV-Tools) is not installed";
  }

  ShaderResult<IrModule> module = programs::BuildSolidFillModule();
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::vector<uint32_t>> spirv = EmitSpirv(module.result());
  ASSERT_FALSE(spirv.hasError()) << "EmitSpirv failed: " << spirv.error();

  const char* testTmpdir = std::getenv("TEST_TMPDIR");
  ASSERT_NE(testTmpdir, nullptr);
  const std::string modulePath = std::string(testTmpdir) + "/solid_fill.spv";
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

  EXPECT_EQ(validationStatus, 0) << "spirv-val rejected the emitted module:\n" << validationOutput;
  EXPECT_THAT(validationOutput, Not(HasSubstr("error"))) << validationOutput;
  if (!validationOutput.empty()) {
    // Surface warnings in the test log even when validation succeeds.
    std::fprintf(stderr, "spirv-val output:\n%s\n", validationOutput.c_str());
  }
}

}  // namespace
}  // namespace donner::gpu::shader
