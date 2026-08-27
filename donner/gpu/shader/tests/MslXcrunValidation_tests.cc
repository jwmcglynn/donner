/// @file
/// Out-of-process MSL validation: every emitted MSL module must compile cleanly with the platform
/// Metal compiler (`xcrun -sdk macosx metal`). Platform compilers run as external verification
/// tools rather than build dependencies.
///
/// A negative control proves the detection mechanism: deliberately invalid MSL must be rejected,
/// so an acceptance result here means the compiler actually inspected the source rather than the
/// harness silently reporting success.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/programs/ColorMatrix.h"
#include "donner/gpu/shader/programs/SolidFill.h"
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

/// Probes for a usable offline Metal compiler, returning a skip reason when one is unavailable.
/// Recent Xcode versions ship it as a downloadable component, so `xcrun --find metal` can succeed
/// while the tool itself is absent; the probe compiles a trivial kernel to detect that case.
std::string FindMetalCompilerSkipReason() {
  // Note: recent Xcode versions ship the offline Metal compiler as a downloadable component
  // (xcodebuild -downloadComponent MetalToolchain); `xcrun --find metal` can succeed while the
  // tool itself is absent, so probe-compile a trivial kernel to detect that case.
  std::string findOutput;
  if (RunCommand("xcrun -sdk macosx --find metal", &findOutput) != 0) {
    return "xcrun / Metal compiler unavailable: " + findOutput;
  }

  const char* probeTmpdir = std::getenv("TEST_TMPDIR");
  if (probeTmpdir == nullptr) {
    return "TEST_TMPDIR is unset";
  }
  const std::string probePath = std::string(probeTmpdir) + "/probe.metal";
  {
    std::ofstream probe(probePath, std::ios::binary | std::ios::trunc);
    if (!probe.good()) {
      return "Failed to write " + probePath;
    }
    probe << "kernel void donnerProbe() {}\n";
  }
  std::string probeOutput;
  const int probeStatus = RunCommand(
      "xcrun -sdk macosx metal -std=metal3.0 -c \"" + probePath + "\" -o \"" + probePath + ".air\"",
      &probeOutput);
  if (probeStatus != 0 || probeOutput.find("missing Metal Toolchain") != std::string::npos) {
    return "Offline Metal compiler unavailable (the runtime Metal framework compiler used by the "
           "vertical slice tests is unaffected): " +
           probeOutput;
  }
  return "";
}

/// Emits \p module as MSL, writes it under TEST_TMPDIR as `<name>.metal`, and asserts the Metal
/// compiler accepts it.
/// @param module Built IR module to emit and compile.
/// @param name Base file name for the emitted source and its object output.
void ExpectCompilesWithMetalCompiler(ShaderResult<IrModule>&& module, const std::string& name) {
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::string> msl = EmitMsl(module.result());
  ASSERT_FALSE(msl.hasError()) << "EmitMsl failed: " << msl.error();

  const char* testTmpdir = std::getenv("TEST_TMPDIR");
  ASSERT_NE(testTmpdir, nullptr);
  const std::string sourcePath = std::string(testTmpdir) + "/" + name + ".metal";
  const std::string outputPath = std::string(testTmpdir) + "/" + name + ".air";
  {
    std::ofstream out(sourcePath, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "Failed to write " << sourcePath;
    out << msl.result();
  }

  std::string compileOutput;
  const int compileStatus = RunCommand(
      "xcrun -sdk macosx metal -std=metal3.0 -c \"" + sourcePath + "\" -o \"" + outputPath + "\"",
      &compileOutput);

  EXPECT_EQ(compileStatus, 0) << "Metal compiler rejected the emitted MSL for " << name << ":\n"
                              << compileOutput;
  EXPECT_THAT(compileOutput, Not(HasSubstr("error:"))) << compileOutput;
  if (!compileOutput.empty()) {
    // Surface warnings in the test log even when compilation succeeds.
    std::fprintf(stderr, "metal compiler output for %s:\n%s\n", name.c_str(),
                 compileOutput.c_str());
  }
}

/// Compiles \p source and returns the compiler's combined output, setting \p status to its exit
/// code. Used by the negative control, which needs the failure rather than an assertion on it.
/// @param source MSL text to compile.
/// @param name Base file name for the source and its object output.
/// @param status Set to the compiler's exit status.
std::string CompileMslForStatus(const std::string& source, const std::string& name, int* status) {
  const char* testTmpdir = std::getenv("TEST_TMPDIR");
  if (testTmpdir == nullptr) {
    *status = -1;
    return "TEST_TMPDIR is unset";
  }
  const std::string sourcePath = std::string(testTmpdir) + "/" + name + ".metal";
  const std::string outputPath = std::string(testTmpdir) + "/" + name + ".air";
  {
    std::ofstream out(sourcePath, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
      *status = -1;
      return "Failed to write " + sourcePath;
    }
    out << source;
  }

  std::string output;
  *status = RunCommand(
      "xcrun -sdk macosx metal -std=metal3.0 -c \"" + sourcePath + "\" -o \"" + outputPath + "\"",
      &output);
  return output;
}

TEST(MslXcrunValidation, APositionOnlyFragmentEntryCompilesWithMetalCompiler) {
  // The emitter omits the [[stage_in]] struct when nothing would go in it, because Metal rejects
  // an empty one. A fragment entry whose only input is the position builtin declares no location,
  // but position has no direct-parameter spelling in this emitter, so omitting the struct would
  // leave the body referencing an input that no parameter carries. The compiler is the check that
  // catches that; a string-shape assertion would not.
  const std::string skipReason = FindMetalCompilerSkipReason();
  if (!skipReason.empty()) {
    GTEST_SKIP() << skipReason;
  }
  ExpectCompilesWithMetalCompiler(BuildPositionOnlyFragmentModule(), "position_only_fragment");
}

TEST(MslXcrunValidation, EmittedSolidFillCompilesWithMetalCompiler) {
  const std::string skipReason = FindMetalCompilerSkipReason();
  if (!skipReason.empty()) {
    GTEST_SKIP() << skipReason;
  }
  ExpectCompilesWithMetalCompiler(programs::BuildSolidFillModule(), "solid_fill");
}

TEST(MslXcrunValidation, EmittedColorMatrixComputeCompilesWithMetalCompiler) {
  const std::string skipReason = FindMetalCompilerSkipReason();
  if (!skipReason.empty()) {
    GTEST_SKIP() << skipReason;
  }
  ExpectCompilesWithMetalCompiler(programs::BuildColorMatrixModule(), "color_matrix");
}

TEST(MslXcrunValidation, NegativeControlDetectsInvalidMsl) {
  // Proves the detection mechanism: MSL the compiler must reject has to come back as a nonzero
  // status with a diagnostic. Without this, a harness that silently reported success would make
  // the positive results above meaningless.
  const std::string skipReason = FindMetalCompilerSkipReason();
  if (!skipReason.empty()) {
    GTEST_SKIP() << skipReason;
  }

  int status = 0;
  const std::string output = CompileMslForStatus(
      "#include <metal_stdlib>\nkernel void broken(device float* out) { out[0] = nonsense; }\n",
      "invalid", &status);

  EXPECT_NE(status, 0) << "the Metal compiler accepted deliberately invalid MSL:\n" << output;
  EXPECT_THAT(output, HasSubstr("error:")) << output;
}

}  // namespace
}  // namespace donner::gpu::shader
