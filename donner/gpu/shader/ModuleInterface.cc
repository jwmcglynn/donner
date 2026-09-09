#include "donner/gpu/shader/ModuleInterface.h"

#include <algorithm>

#include "donner/gpu/shader/IrLayout.h"

namespace donner::gpu::shader {
namespace {

struct FunctionReferences {
  std::vector<IrExpr::RefInfo> names;
  std::vector<RcString> calls;
};

void CollectReferences(const IrStmt& statement, FunctionReferences& references) {
  const IrStmt::Data& data = statement.data();
  for (const IrExpr& expression : data.exprs) {
    expression.collectRefs(references.names);
    expression.collectUserCalls(references.calls);
  }
  for (const IrStmt& child : data.body) {
    CollectReferences(child, references);
  }
  for (const IrStmt& child : data.elseBody) {
    CollectReferences(child, references);
  }
  if (data.init) {
    CollectReferences(*data.init, references);
  }
  if (data.continuing) {
    CollectReferences(*data.continuing, references);
  }
}

void CollectUsedBindings(const IrModule& module, size_t functionIndex, std::vector<bool>& visited,
                         std::vector<bool>& used) {
  if (visited[functionIndex]) {
    return;
  }
  visited[functionIndex] = true;
  FunctionReferences references;
  for (const IrStmt& statement : module.functions()[functionIndex].body) {
    CollectReferences(statement, references);
  }
  for (const IrExpr::RefInfo& reference : references.names) {
    if (reference.kind != RefKind::Resource) {
      continue;
    }
    for (size_t index = 0; index < module.bindings().size(); ++index) {
      used[index] = used[index] || module.bindings()[index].name == reference.name;
    }
  }
  for (const RcString& call : references.calls) {
    const auto function = std::ranges::find(module.functions(), call, &IrFunction::name);
    if (function != module.functions().end()) {
      CollectUsedBindings(module, function - module.functions().begin(), visited, used);
    }
  }
}

ShaderStage RuntimeStage(StageKind stage) {
  switch (stage) {
    case StageKind::None: return ShaderStage::None;
    case StageKind::Vertex: return ShaderStage::Vertex;
    case StageKind::Fragment: return ShaderStage::Fragment;
    case StageKind::Compute: return ShaderStage::Compute;
  }
  return ShaderStage::None;
}

ShaderResult<ShaderBufferBindingInfo> BufferInfo(const IrBinding& binding,
                                                 const IrFunction& function) {
  const bool uniform = binding.kind == BindingKind::UniformBuffer;
  ShaderBufferBindingInfo info{
      function.name, RuntimeStage(function.stage), binding.group, binding.binding,
      uniform ? BindingType::UniformBuffer : BindingType::ReadOnlyStorageBuffer};
  const AddressSpace addressSpace = uniform ? AddressSpace::Uniform : AddressSpace::Storage;
  if (binding.type.kind() == IrType::Kind::RuntimeArray) {
    auto stride = ComputeArrayStride(binding.type, addressSpace);
    if (stride.hasError()) {
      return std::move(stride).error();
    }
    info.minSizeBytes = info.runtimeArrayStrideBytes = stride.result();
  } else {
    auto layout = ComputeTypeLayout(binding.type, addressSpace);
    if (layout.hasError()) {
      return std::move(layout).error();
    }
    info.minSizeBytes = layout.result().sizeBytes;
  }
  return info;
}

}  // namespace

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

ShaderResult<std::vector<ShaderBufferBindingInfo>> BufferBindingsOf(const IrModule& module) {
  std::vector<ShaderBufferBindingInfo> result;
  for (size_t index = 0; index < module.functions().size(); ++index) {
    const IrFunction& function = module.functions()[index];
    if (function.stage == StageKind::None) {
      continue;
    }
    std::vector<bool> used(module.bindings().size());
    std::vector<bool> visited(module.functions().size());
    CollectUsedBindings(module, index, visited, used);
    for (size_t bindingIndex = 0; bindingIndex < module.bindings().size(); ++bindingIndex) {
      const IrBinding& binding = module.bindings()[bindingIndex];
      if (!used[bindingIndex] || (binding.kind != BindingKind::UniformBuffer &&
                                  binding.kind != BindingKind::ReadOnlyStorageBuffer)) {
        continue;
      }
      auto info = BufferInfo(binding, function);
      if (info.hasError()) {
        return std::move(info).error();
      }
      result.push_back(std::move(info).result());
    }
  }
  return result;
}

}  // namespace donner::gpu::shader
