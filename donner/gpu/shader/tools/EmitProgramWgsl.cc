/// @file
/// Emits build-time WGSL or a complete native/WebGPU shader descriptor header.
///
/// The editor's WebAssembly package is the reason this exists. Constructing a program's WGSL at
/// runtime links the IR and the WGSL emitter into whatever binary does it, which costs the editor
/// package more than its size budget allows for a string that is identical on every run. Emitting
/// at build time keeps the IR module as the single source of truth while the runtime links
/// neither the IR nor the emitters.
///
/// Program-name driven rather than one tool per program: every filter family migrating onto the
/// IR needs the same treatment, and nineteen copies of this file is not a mechanism.
///
/// Generated source stays a build artifact. Compiler validation and execution tests check its
/// interface and behavior; deterministic-emission tests detect unstable generation without
/// committing full shader snapshots.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/Checkerboard.h"
#include "donner/gpu/shader/programs/ColorSpaceConvert.h"
#include "donner/gpu/shader/programs/ComponentTransfer.h"
#include "donner/gpu/shader/programs/Composite.h"
#include "donner/gpu/shader/programs/DisplacementMap.h"
#include "donner/gpu/shader/programs/ConvolveMatrix.h"
#include "donner/gpu/shader/programs/DropShadow.h"
#include "donner/gpu/shader/programs/FilterColorMatrix.h"
#include "donner/gpu/shader/programs/FilterImage.h"
#include "donner/gpu/shader/programs/Flood.h"
#include "donner/gpu/shader/programs/GaussianBlur.h"
#include "donner/gpu/shader/programs/Lighting.h"
#include "donner/gpu/shader/programs/Merge.h"
#include "donner/gpu/shader/programs/Morphology.h"
#include "donner/gpu/shader/programs/Offset.h"
#include "donner/gpu/shader/programs/SnapshotUnpremultiply.h"
#include "donner/gpu/shader/programs/SubregionClip.h"
#include "donner/gpu/shader/programs/Tile.h"
#include "donner/gpu/shader/programs/Turbulence.h"

namespace donner::gpu::shader {
namespace {

/// One program this tool can emit, named as the build files name it.
struct ProgramEntry {
  std::string_view name;              //!< Build-facing program identifier.
  size_t expectedComputeEntryPoints;  //!< Zero for a render-only program.
  ShaderResult<IrModule> (*build)();  //!< Builder for the program's IR module.
};

/// Programs this tool knows how to emit. A new IR program adds one row.
constexpr ProgramEntry kPrograms[] = {
    {"checkerboard", 0, &programs::BuildCheckerboardModule},
    {"color_space_convert", 1, &programs::BuildColorSpaceConvertModule},
    {"filter_color_matrix", 1, &programs::BuildFilterColorMatrixModule},
    {"filter_image", 1, &programs::BuildFilterImageModule},
    {"flood", 1, &programs::BuildFloodModule},
    {"gaussian_blur", 1, &programs::BuildGaussianBlurModule},
    {"diffuse_lighting", 1, &programs::BuildDiffuseLightingModule},
    {"specular_lighting", 1, &programs::BuildSpecularLightingModule},
    {"merge", 1, &programs::BuildMergeModule},
    {"morphology", 1, &programs::BuildMorphologyModule},
    {"composite", 1, &programs::BuildCompositeModule},
    {"component_transfer", 1, &programs::BuildComponentTransferModule},
    {"displacement_map", 1, &programs::BuildDisplacementMapModule},
    {"drop_shadow", 1, &programs::BuildDropShadowModule},
    {"convolve_matrix", 1, &programs::BuildConvolveMatrixModule},
    {"offset", 1, &programs::BuildOffsetModule},
    {"tile", 1, &programs::BuildTileModule},
    {"turbulence", 1, &programs::BuildTurbulenceModule},
    {"snapshot_unpremultiply", 1, &programs::BuildSnapshotUnpremultiplyModule},
    {"subregion_clip", 1, &programs::BuildSubregionClipModule},
    {"filter_resolve", 1, &programs::BuildFilterResolveModule},
};

/// Writes \p contents to \p path, returning false with a diagnostic on failure.
bool WriteFile(const std::string& path, const std::string& contents) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.good()) {
    std::fprintf(stderr, "emit_program_wgsl: cannot open %s for writing\n", path.c_str());
    return false;
  }
  out << contents;
  return out.good();
}

/// Finds a matching catalog entry, whose storage lasts for the process lifetime.
/// @param program Catalog program name.
const ProgramEntry* FindProgram(std::string_view program) {
  const ProgramEntry* found = nullptr;
  for (const ProgramEntry& entry : kPrograms) {
    if (entry.name == program) {
      found = &entry;
    }
  }
  return found;
}

/// Writes source cases with their platform guards and unavailable-kind refusal.
/// @param header Destination header stream.
/// @param wgsl Generated WGSL source.
/// @param msl Generated MSL source.
/// @param spirv Generated SPIR-V words.
void WriteDescriptorSources(std::ostream& header, std::string_view wgsl, std::string_view msl,
                            std::span<const uint32_t> spirv) {
  header << "switch (kind) {\n"
         << "case ShaderSourceKind::Wgsl: descriptor.sourceText = R\"shader(" << wgsl
         << ")shader\"; break;\n"
         << "#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)\n"
         << "case ShaderSourceKind::Msl: descriptor.sourceText = R\"shader(" << msl
         << ")shader\"; break;\n#endif\n"
         << "#if defined(__linux__) && !defined(__EMSCRIPTEN__)\n"
         << "case ShaderSourceKind::Spirv: descriptor.spirvWords = {";
  for (uint32_t word : spirv) {
    header << word << "u,";
  }
  header << "}; break;\n#endif\ndefault: return descriptor;\n}\n";
}

/// Writes the descriptor's buffer requirements derived from the shader IR.
/// @param header Destination header stream.
/// @param bindings Buffer requirements for every entry point.
void WriteDescriptorBufferBindings(std::ostream& header,
                                   std::span<const ShaderBufferBindingInfo> bindings) {
  header << "descriptor.bufferBindings = std::vector<ShaderBufferBindingInfo>{\n";
  for (const ShaderBufferBindingInfo& binding : bindings) {
    header << "{\"" << binding.entryPoint << "\", ShaderStage::" << binding.stage << ", "
           << binding.group << "u," << binding.binding << "u, BindingType::" << binding.type << ","
           << binding.minSizeBytes << "u," << binding.runtimeArrayStrideBytes << "u},\n";
  }
  header << "};\n";
}

/// Writes compute entry-point names and workgroup dimensions.
/// @param header Destination header stream.
/// @param entryPoints Entry points derived from the shader IR.
void WriteDescriptorEntryPoints(std::ostream& header,
                                std::span<const ComputeEntryPointInfo> entryPoints) {
  header << "descriptor.computeEntryPoints = {\n";
  for (const ComputeEntryPointInfo& entry : entryPoints) {
    header << "{\"" << entry.name << "\", {" << entry.workgroupSize.x << "u,"
           << entry.workgroupSize.y << "u," << entry.workgroupSize.z << "u}},\n";
  }
  header << "};\n";
}

/// Generates native sources and writes a complete runtime descriptor header.
/// @param program Catalog program name used as the generated namespace and label.
/// @param module Validated shader IR module.
/// @param wgsl Previously emitted WGSL source.
/// @param entryPoints Validated compute entry-point interface.
/// @param outputPath Destination header path.
bool WriteDescriptorHeader(std::string_view program, const IrModule& module, std::string_view wgsl,
                           std::span<const ComputeEntryPointInfo> entryPoints,
                           const std::string& outputPath) {
  ShaderResult<std::string> msl = EmitMsl(module);
  ShaderResult<std::vector<uint32_t>> spirv = EmitSpirv(module);
  ShaderResult<std::vector<ShaderBufferBindingInfo>> bindings = BufferBindingsOf(module);
  if (msl.hasError() || spirv.hasError() || bindings.hasError()) {
    std::fprintf(stderr, "emit_program_wgsl: native artifact generation failed\n");
    return false;
  }

  std::ostringstream header;
  header << "#pragma once\n#include \"donner/gpu/Descriptors.h\"\n"
         << "namespace donner::gpu::generated::" << program << " {\n"
         << "inline ShaderModuleDescriptor BuildDescriptor(ShaderSourceKind kind) {\n"
         << "ShaderModuleDescriptor descriptor;\ndescriptor.label = \"" << program << "\";\n"
         << "descriptor.sourceKind = kind;\n";
  WriteDescriptorSources(header, wgsl, msl.result(), spirv.result());
  WriteDescriptorBufferBindings(header, bindings.result());
  WriteDescriptorEntryPoints(header, entryPoints);
  header << "return descriptor;\n}\n}\n";
  return WriteFile(outputPath, header.str());
}

int Run(std::string_view program, const std::string& wgslPath, bool descriptorHeader) {
  const ProgramEntry* found = FindProgram(program);
  if (found == nullptr) {
    std::fprintf(stderr, "emit_program_wgsl: unknown program \"%.*s\"\n",
                 static_cast<int>(program.size()), program.data());
    return 1;
  }

  ShaderResult<IrModule> module = found->build();
  if (module.hasError()) {
    std::fprintf(stderr, "emit_program_wgsl: building %.*s failed\n",
                 static_cast<int>(program.size()), program.data());
    return 1;
  }
  ShaderResult<std::string> wgsl = EmitWgsl(module.result());
  if (wgsl.hasError()) {
    std::fprintf(stderr, "emit_program_wgsl: emitting %.*s failed\n",
                 static_cast<int>(program.size()), program.data());
    return 1;
  }

  const std::vector<ComputeEntryPointInfo> entryPoints = ComputeEntryPointsOf(module.result());
  if (entryPoints.size() != found->expectedComputeEntryPoints) {
    std::fprintf(stderr,
                 "emit_program_wgsl: %.*s declares %zu compute entry points; this tool emits "
                 "an unexpected entry-point count\n",
                 static_cast<int>(program.size()), program.data(), entryPoints.size());
    return 1;
  }

  const bool written =
      descriptorHeader
          ? WriteDescriptorHeader(program, module.result(), wgsl.result(), entryPoints, wgslPath)
          : WriteFile(wgslPath, wgsl.result());
  return written ? 0 : 1;
}

}  // namespace
}  // namespace donner::gpu::shader

int main(int argc, char** argv) {
  if (argc != 3 && (argc != 4 || std::strcmp(argv[3], "--descriptor-header") != 0)) {
    std::fprintf(stderr, "usage: emit_program_wgsl <program> <output> [--descriptor-header]\n");
    return 1;
  }
  return donner::gpu::shader::Run(argv[1], argv[2], argc == 4);
}
