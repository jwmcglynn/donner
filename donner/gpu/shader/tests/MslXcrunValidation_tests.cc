/// @file
/// Out-of-process MSL validation: every emitted MSL module must compile cleanly with the platform
/// Metal compiler (`xcrun -sdk macosx metal`). Platform compilers run as external verification
/// tools rather than build dependencies, so a developer machine without the offline compiler skips
/// these cases and an automated lane without it fails them.
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
#include <string_view>

#include "donner/gpu/shader/MslEmitter.h"
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
#include "donner/gpu/shader/tests/ExternalToolGate.h"
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

/// How the gate names this compiler, spelled the way a person would install it.
constexpr std::string_view kMetalCompilerToolName =
    "the offline Metal compiler (xcodebuild -downloadComponent MetalToolchain)";

/// Probes for a usable offline Metal compiler, returning what it found when there is none.
/// Recent Xcode versions ship it as a downloadable component, so `xcrun --find metal` can succeed
/// while the tool itself is absent; the probe compiles a trivial kernel to detect that case.
std::string FindMetalCompilerUnavailableReason() {
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

/// Writes \p msl under TEST_TMPDIR as `<name>.metal` and asserts the Metal compiler accepts it.
/// @param msl MSL source to compile.
/// @param name Base file name for the emitted source and its object output.
void ExpectCompilesWithMetalCompiler(std::string_view msl, const std::string& name) {
  const char* testTmpdir = std::getenv("TEST_TMPDIR");
  ASSERT_NE(testTmpdir, nullptr);
  const std::string sourcePath = std::string(testTmpdir) + "/" + name + ".metal";
  const std::string outputPath = std::string(testTmpdir) + "/" + name + ".air";
  {
    std::ofstream out(sourcePath, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "Failed to write " << sourcePath;
    out.write(msl.data(), static_cast<std::streamsize>(msl.size()));
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

/// Emits \p module and verifies the resulting MSL with the platform compiler.
void ExpectCompilesWithMetalCompiler(ShaderResult<IrModule>&& module, const std::string& name) {
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::string> msl = EmitMsl(module.result());
  ASSERT_FALSE(msl.hasError()) << "EmitMsl failed: " << msl.error();
  ExpectCompilesWithMetalCompiler(msl.result(), name);
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

TEST(MslXcrunValidation, EmittedCheckerboardCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::CheckerboardAllProjections().msl, "checkerboard");
  ExpectCompilesWithMetalCompiler(tests::CheckerboardMutatedAllProjections().msl,
                                  "checkerboard_mutated");
}

TEST(MslXcrunValidation, FinalFilterResolveCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::FilterResolveAllProjections().msl, "filter_resolve");
}
TEST(MslXcrunValidation, EmittedCompositeCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::CompositeAllProjections().msl, "composite");
  ExpectCompilesWithMetalCompiler(tests::CompositeMutatedAllProjections().msl, "composite_mutated");
}

TEST(MslXcrunValidation, CompiledGraphicsEntriesPassMetalCompilation) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(wgsl::tests::GraphicsShader().msl, "compiled_graphics");
  ExpectCompilesWithMetalCompiler(wgsl::tests::MatrixShader().msl, "compiled_matrices");
  ExpectCompilesWithMetalCompiler(wgsl::tests::StorageArrayShader().msl, "storage_arrays");
  ExpectCompilesWithMetalCompiler(wgsl::tests::ControlShader().msl, "numeric_control");
  ExpectCompilesWithMetalCompiler(tests::SlugMaskAllProjections().msl, "slug_mask");
  ExpectCompilesWithMetalCompiler(wgsl::tests::MatrixOperationsShader().msl, "matrix_operations");
}

TEST(MslXcrunValidation, EmittedConvolveMatrixCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::ConvolveMatrixAllProjections().msl, "convolve_matrix");
}

TEST(MslXcrunValidation, EmittedMergeCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::MergeAllProjections().msl, "merge");
  ExpectCompilesWithMetalCompiler(tests::MergeMutatedAllProjections().msl, "merge_mutated");
}

TEST(MslXcrunValidation, APositionOnlyFragmentEntryCompilesWithMetalCompiler) {
  // The emitter omits the [[stage_in]] struct when nothing would go in it, because Metal rejects
  // an empty one. A fragment entry whose only input is the position builtin declares no location,
  // but position has no direct-parameter spelling in this emitter, so omitting the struct would
  // leave the body referencing an input that no parameter carries. The compiler is the check that
  // catches that; a string-shape assertion would not.
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(BuildPositionOnlyFragmentModule(), "position_only_fragment");
}

TEST(MslXcrunValidation, EmittedSolidFillCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(programs::BuildSolidFillModule(), "solid_fill");
}

TEST(MslXcrunValidation, EmittedColorMatrixComputeCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(programs::BuildColorMatrixModule(), "color_matrix");
}

TEST(MslXcrunValidation, EmittedSnapshotUnpremultiplyComputeCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  // This program emits a componentwise comparison, a bool-vector reduction and unsigned
  // division, so the compiler confirms those spellings are real MSL.
  ExpectCompilesWithMetalCompiler(tests::SnapshotUnpremultiplyAllProjections().msl,
                                  "snapshot_unpremultiply");
  ExpectCompilesWithMetalCompiler(tests::SnapshotUnpremultiplyMutatedAllProjections().msl,
                                  "snapshot_unpremultiply_mutated");
}

TEST(MslXcrunValidation, EmittedBoolVectorReductionsCompileWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  // `all` reaches no shipping program, so without this the compiler would never see it and a
  // wrong spelling would only ever be checked against the emitter's own idea of it.
  ExpectCompilesWithMetalCompiler(BuildVectorReductionModule(), "vector_reductions");
}

TEST(MslXcrunValidation, EmittedMathPrimitivesCompileWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  // This fixture also exercises vector sign and floor, which scalar offset does not reach.
  ExpectCompilesWithMetalCompiler(BuildMathPrimitiveModule(), "math_primitives");
}

TEST(MslXcrunValidation, EmittedFloodComputeCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::FloodAllProjections().msl, "flood");
  ExpectCompilesWithMetalCompiler(tests::FloodMutatedAllProjections().msl, "flood_mutated");
}

TEST(MslXcrunValidation, EmittedSubregionClipComputeCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::SubregionClipAllProjections().msl, "subregion_clip");
  ExpectCompilesWithMetalCompiler(tests::SubregionClipMutatedAllProjections().msl,
                                  "subregion_clip_mutated");
}

TEST(MslXcrunValidation, EmittedFilterColorMatrixCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::FilterColorMatrixAllProjections().msl,
                                  "filter_color_matrix");
  ExpectCompilesWithMetalCompiler(tests::FilterColorMatrixMutatedAllProjections().msl,
                                  "filter_color_matrix_mutated");
}

TEST(MslXcrunValidation, EmittedFilterImageCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::FilterImageAllProjections().msl, "filter_image");
  ExpectCompilesWithMetalCompiler(tests::FilterImageMutatedAllProjections().msl,
                                  "filter_image_mutated");
}

TEST(MslXcrunValidation, EmittedOffsetComputeCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  // The first compute program to call a function of its own, so this is where the compiler
  // confirms the declaration order the emitter writes is one Metal accepts for a kernel.
  ExpectCompilesWithMetalCompiler(tests::OffsetAllProjections().msl, "offset");
  ExpectCompilesWithMetalCompiler(tests::FloorAllProjections().msl, "wgsl_floor");
  ExpectCompilesWithMetalCompiler(tests::SignAllProjections().msl, "wgsl_sign");
}

TEST(MslXcrunValidation, EmittedTileComputeCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  // The first compute program to call a function of its own, so this is where the compiler
  // confirms the declaration order the emitter writes is one Metal accepts for a kernel.
  ExpectCompilesWithMetalCompiler(tests::TileAllProjections().msl, "tile");
  ExpectCompilesWithMetalCompiler(tests::TileMutatedAllProjections().msl, "tile_mutated");
}

TEST(MslXcrunValidation, EmittedComponentTransferComputeCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  // The first compute program to call a function of its own, so this is where the compiler
  // confirms the declaration order the emitter writes is one Metal accepts for a kernel.
  ExpectCompilesWithMetalCompiler(tests::ComponentTransferAllProjections().msl,
                                  "component_transfer");
  ExpectCompilesWithMetalCompiler(tests::ComponentTransferMutatedAllProjections().msl,
                                  "component_transfer_mutated");
}

TEST(MslXcrunValidation, EmittedTurbulenceComputeCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::TurbulenceAllProjections().msl, "turbulence");
  ExpectCompilesWithMetalCompiler(tests::EightArgumentCallAllProjections().msl, "eight_arguments");
}

TEST(MslXcrunValidation, EmittedDropShadowComputeCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  // The first compute program to call a function of its own, so this is where the compiler
  // confirms the declaration order the emitter writes is one Metal accepts for a kernel.
  ExpectCompilesWithMetalCompiler(tests::DropShadowAllProjections().msl, "drop_shadow");
  ExpectCompilesWithMetalCompiler(tests::DropShadowMutatedAllProjections().msl,
                                  "drop_shadow_mutated");
}

TEST(MslXcrunValidation, EmittedDisplacementMapComputeCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::DisplacementMapAllProjections().msl, "displacement_map");
  ExpectCompilesWithMetalCompiler(tests::DisplacementMapMutatedAllProjections().msl,
                                  "displacement_map_mutated");
}

TEST(MslXcrunValidation, EmittedGaussianBlurComputeCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::GaussianBlurAllProjections().msl, "gaussian_blur");
}

TEST(MslXcrunValidation, EmittedMorphologyComputeCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  // The first compute program to call a function of its own, so this is where the compiler
  // confirms the declaration order the emitter writes is one Metal accepts for a kernel.
  ExpectCompilesWithMetalCompiler(tests::MorphologyAllProjections().msl, "morphology");
  ExpectCompilesWithMetalCompiler(tests::MorphologyMutatedAllProjections().msl,
                                  "morphology_mutated");
}

TEST(MslXcrunValidation, EmittedLightingComputesCompileWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::DiffuseLightingAllProjections().msl, "diffuse_lighting");
  ExpectCompilesWithMetalCompiler(tests::DiffuseLightingMutatedAllProjections().msl,
                                  "diffuse_lighting_mutated");
  ExpectCompilesWithMetalCompiler(tests::LightingMathAllProjections().msl, "lighting_vector_math");
  ExpectCompilesWithMetalCompiler(tests::SpecularLightingAllProjections().msl, "specular_lighting");
}

TEST(MslXcrunValidation, FloatStorageTextureCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(BuildFloatStorageModule(), "float_storage");
}

TEST(MslXcrunValidation, EmittedColorSpaceConvertCompilesWithMetalCompiler) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::ColorSpaceConvertAllProjections().msl,
                                  "color_space_convert");
  ExpectCompilesWithMetalCompiler(tests::ColorSpaceConvertMutatedAllProjections().msl,
                                  "color_space_convert_mutated");
}

TEST(MslXcrunValidation, NegativeControlDetectsInvalidMsl) {
  // Proves the detection mechanism: MSL the compiler must reject has to come back as a nonzero
  // status with a diagnostic. Without this, a harness that silently reported success would make
  // the positive results above meaningless.
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());

  int status = 0;
  const std::string output = CompileMslForStatus(
      "#include <metal_stdlib>\nkernel void broken(device float* out) { out[0] = nonsense; }\n",
      "invalid", &status);

  EXPECT_NE(status, 0) << "the Metal compiler accepted deliberately invalid MSL:\n" << output;
  EXPECT_THAT(output, HasSubstr("error:")) << output;
}

TEST(MslXcrunValidation, ImageBlit) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::ImageBlitAllProjections().msl, "ImageBlit");
}
TEST(MslXcrunValidation, ImageBlitMutated) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::ImageBlitMutatedAllProjections().msl, "ImageBlitMutated");
}
TEST(MslXcrunValidation, ImageBlitExplicitLevel) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::ImageBlitExplicitLevelAllProjections().msl,
                                  "ImageBlitExplicitLevel");
}
TEST(MslXcrunValidation, ArraySwitch) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::ArraySwitchAllProjections().msl, "ArraySwitch");
}
TEST(MslXcrunValidation, LoopSwitch) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::LoopSwitchAllProjections().msl, "LoopSwitch");
}
TEST(MslXcrunValidation, StructConstruction) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::StructConstructionAllProjections().msl,
                                  "StructConstruction");
}

TEST(MslXcrunValidation, VectorMix) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::VectorMixAllProjections().msl, "VectorMix");
}

TEST(MslXcrunValidation, SlugGradient) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::SlugGradientAllProjections().msl, "slug_gradient");
}
TEST(MslXcrunValidation, SlugGradientMutated) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::SlugGradientMutatedAllProjections().msl,
                                  "slug_gradientMutated");
}
TEST(MslXcrunValidation, FilterBlend) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::FilterBlendAllProjections().msl, "filter_blend");
}
TEST(MslXcrunValidation, FilterBlendMutated) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::FilterBlendMutatedAllProjections().msl,
                                  "filter_blendMutated");
}

TEST(MslXcrunValidation, SlugFill) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::SlugFillAllProjections().msl, "slug_fill");
}
TEST(MslXcrunValidation, SlugFillMutated) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::SlugFillMutatedAllProjections().msl, "slug_fillMutated");
}

TEST(MslXcrunValidation, SlugFlatInterface) {
  DONNER_REQUIRE_EXTERNAL_TOOL(kMetalCompilerToolName, FindMetalCompilerUnavailableReason());
  ExpectCompilesWithMetalCompiler(tests::SlugFlatInterfaceAllProjections().msl, "slug_flat");
}

}  // namespace
}  // namespace donner::gpu::shader
