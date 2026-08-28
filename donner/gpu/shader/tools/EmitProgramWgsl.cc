/// @file
/// Emits a shader IR program's WGSL, and the entry-point facts a host needs to create a pipeline
/// from it, as build-time artifacts.
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
/// The committed byte-exact goldens under `tests/testdata` stay as they are. They pin the same
/// `EmitWgsl` output this tool writes, so a golden and a generated artifact that disagreed would
/// fail the program's golden test; keeping the golden a reviewed, committed file is what makes an
/// emitter change visible in a diff.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/SnapshotUnpremultiply.h"

namespace donner::gpu::shader {
namespace {

/// One program this tool can emit, named as the build files name it.
struct ProgramEntry {
  std::string_view name;              //!< Build-facing program identifier.
  ShaderResult<IrModule> (*build)();  //!< Builder for the program's IR module.
};

/// Programs this tool knows how to emit. A new IR program adds one row.
constexpr ProgramEntry kPrograms[] = {
    {"snapshot_unpremultiply", &programs::BuildSnapshotUnpremultiplyModule},
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

int Run(std::string_view program, const std::string& wgslPath) {
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
  if (entryPoints.size() != 1) {
    std::fprintf(stderr,
                 "emit_program_wgsl: %.*s declares %zu compute entry points; this tool emits "
                 "constants for exactly one\n",
                 static_cast<int>(program.size()), program.data(), entryPoints.size());
    return 1;
  }

  return WriteFile(wgslPath, wgsl.result()) ? 0 : 1;
}

}  // namespace
}  // namespace donner::gpu::shader

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: emit_program_wgsl <program> <out.wgsl>\n");
    return 1;
  }
  return donner::gpu::shader::Run(argv[1], argv[2]);
}
