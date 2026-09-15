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
  result.bufferBindings.emplace();
  for (const ShaderEntryPoint& entry : shader.entryPoints) {
    if (entry.stage == ShaderStage::Compute)
      result.computeEntryPoints.push_back(
          {RcString(entry.name.view()),
           {entry.workgroupSize[0], entry.workgroupSize[1], entry.workgroupSize[2]}});
    for (size_t i = 0; i < shader.resources.size() && i < 32; ++i) {
      if ((entry.resourceMask & (1u << i)) == 0) continue;
      const ShaderResource& resource = shader.resources[i];
      if (resource.type == BindingType::UniformBuffer ||
          resource.type == BindingType::ReadOnlyStorageBuffer)
        result.bufferBindings->push_back({RcString(entry.name.view()), entry.stage, resource.group,
                                          resource.binding, resource.type, resource.minSizeBytes,
                                          resource.runtimeArrayStrideBytes});
    }
  }
  return result;
}

std::vector<BindGroupLayoutEntry> MakeBindingLayout(const CompiledShaderView& shader) {
  std::vector<BindGroupLayoutEntry> result;
  result.reserve(shader.resources.size());
  for (size_t i = 0; i < shader.resources.size() && i < 32; ++i) {
    ShaderStage stages = ShaderStage::None;
    for (const ShaderEntryPoint& entry : shader.entryPoints)
      if ((entry.resourceMask & (1u << i)) != 0) stages |= entry.stage;
    if (stages == ShaderStage::None) continue;
    const ShaderResource& resource = shader.resources[i];
    result.push_back({resource.binding, stages, resource.type, resource.storageFormat});
  }
  return result;
}

}  // namespace donner::gpu::shader
