#include "donner/gpu/shader/ModuleInterface.h"

namespace donner::gpu::shader {

std::vector<ComputeEntryPointInfo> ComputeEntryPointsOf(const IrModule& module) {
  std::vector<ComputeEntryPointInfo> entryPoints;
  for (const IrFunction& function : module.functions()) {
    if (function.stage != StageKind::Compute || !function.workgroupSize) {
      continue;
    }
    // Qualified: the IR declares its own WorkgroupSize in this namespace, and the descriptor the
    // runtime consumes is a different type with the same name one namespace out.
    entryPoints.push_back(ComputeEntryPointInfo{
        function.name,
        ::donner::gpu::WorkgroupSize{function.workgroupSize->x, function.workgroupSize->y,
                                     function.workgroupSize->z}});
  }
  return entryPoints;
}

}  // namespace donner::gpu::shader
