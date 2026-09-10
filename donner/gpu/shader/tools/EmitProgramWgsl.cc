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
#include "donner/gpu/shader/programs/Composite.h"
#include "donner/gpu/shader/programs/FilterColorMatrix.h"
#include "donner/gpu/shader/programs/Flood.h"
#include "donner/gpu/shader/programs/Merge.h"
#include "donner/gpu/shader/programs/Morphology.h"
#include "donner/gpu/shader/programs/Offset.h"
#include "donner/gpu/shader/programs/SnapshotUnpremultiply.h"
#include "donner/gpu/shader/programs/SubregionClip.h"
#include "donner/gpu/shader/programs/Tile.h"

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
    {"flood", 1, &programs::BuildFloodModule},
    {"merge", 1, &programs::BuildMergeModule},
    {"morphology", 1, &programs::BuildMorphologyModule},
    {"composite", 1, &programs::BuildCompositeModule},
    {"offset", 1, &programs::BuildOffsetModule},
    {"tile", 1, &programs::BuildTileModule},
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

int Run(std::string_view program, const std::string& wgslPath, bool descriptorHeader) {
  const ProgramEntry* found = nullptr;
  for (const ProgramEntry& entry : kPrograms) {
    if (entry.name == program) {
      found = &entry;
    }
  }
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

  if (descriptorHeader) {
    ShaderResult<std::string> msl = EmitMsl(module.result());
    ShaderResult<std::vector<uint32_t>> spirv = EmitSpirv(module.result());
    ShaderResult<std::vector<ShaderBufferBindingInfo>> bindings = BufferBindingsOf(module.result());
    if (msl.hasError() || spirv.hasError() || bindings.hasError()) {
      std::fprintf(stderr, "emit_program_wgsl: native artifact generation failed\n");
      return 1;
    }
    std::ostringstream header;
    header << "#pragma once\n#include \"donner/gpu/Descriptors.h\"\n"
           << "namespace donner::gpu::generated::" << program << " {\n"
           << "inline ShaderModuleDescriptor BuildDescriptor(ShaderSourceKind kind) {\n"
           << "ShaderModuleDescriptor descriptor;\ndescriptor.label = \"" << program << "\";\n"
           << "descriptor.sourceKind = kind;\nswitch (kind) {\n"
           << "case ShaderSourceKind::Wgsl: descriptor.sourceText = R\"shader(" << wgsl.result()
           << ")shader\"; break;\n"
           << "#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)\n"
           << "case ShaderSourceKind::Msl: descriptor.sourceText = R\"shader(" << msl.result()
           << ")shader\"; break;\n#endif\n"
           << "#if defined(__linux__) && !defined(__EMSCRIPTEN__)\n"
           << "case ShaderSourceKind::Spirv: descriptor.spirvWords = {";
    for (uint32_t word : spirv.result()) {
      header << word << "u,";
    }
    header << "}; break;\n#endif\ndefault: return descriptor;\n}\n"
           << "descriptor.bufferBindings = std::vector<ShaderBufferBindingInfo>{\n";
    for (const ShaderBufferBindingInfo& binding : bindings.result()) {
      header << "{\"" << binding.entryPoint << "\", ShaderStage::" << binding.stage << ", "
             << binding.group << "u," << binding.binding << "u, BindingType::" << binding.type
             << "," << binding.minSizeBytes << "u," << binding.runtimeArrayStrideBytes << "u},\n";
    }
    header << "};\ndescriptor.computeEntryPoints = {\n";
    for (const ComputeEntryPointInfo& entry : entryPoints) {
      header << "{\"" << entry.name << "\", {" << entry.workgroupSize.x << "u,"
             << entry.workgroupSize.y << "u," << entry.workgroupSize.z << "u}},\n";
    }
    header << "};\nreturn descriptor;\n}\n}\n";
    return WriteFile(wgslPath, header.str()) ? 0 : 1;
  }

  return WriteFile(wgslPath, wgsl.result()) ? 0 : 1;
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
