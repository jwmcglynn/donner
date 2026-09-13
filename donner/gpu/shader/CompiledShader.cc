#include "donner/gpu/shader/CompiledShader.h"

namespace donner::gpu::shader {

ShaderModuleDescriptor MakeShaderDescriptor(const CompiledShaderView& shader, ShaderSourceKind kind,
                                            std::string_view label) {
  ShaderModuleDescriptor result;
  result.label = RcString(label);
  result.sourceKind = kind;
  switch (kind) {
    case ShaderSourceKind::Wgsl: result.sourceText = RcString(shader.wgsl); break;
    case ShaderSourceKind::Msl: result.sourceText = RcString(shader.msl); break;
    case ShaderSourceKind::Spirv:
      result.spirvWords.assign(shader.spirv.begin(), shader.spirv.end());
      break;
  }
  if (result.sourceText.empty() && result.spirvWords.empty()) return result;
  result.computeEntryPoints.push_back(
      {RcString(shader.entryPoint.view()),
       {shader.workgroupSize[0], shader.workgroupSize[1], shader.workgroupSize[2]}});
  result.bufferBindings.emplace();
  for (const ShaderResource& resource : shader.resources) {
    if (resource.type == BindingType::UniformBuffer ||
        resource.type == BindingType::ReadOnlyStorageBuffer)
      result.bufferBindings->push_back({RcString(shader.entryPoint.view()), ShaderStage::Compute,
                                        resource.group, resource.binding, resource.type,
                                        resource.minSizeBytes, 0});
  }
  return result;
}

std::vector<BindGroupLayoutEntry> MakeComputeBindingLayout(const CompiledShaderView& shader) {
  std::vector<BindGroupLayoutEntry> result;
  result.reserve(shader.resources.size());
  for (const ShaderResource& resource : shader.resources)
    result.push_back(
        {resource.binding, ShaderStage::Compute, resource.type, resource.storageFormat});
  return result;
}

}  // namespace donner::gpu::shader
