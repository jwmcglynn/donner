#pragma once
/// @file
/// C++20 immediate compilation of inline WGSL into shader bytes and resource metadata.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/wgsl/Parser.h"
#include "donner/gpu/shader/wgsl/Projection.h"
#include "donner/gpu/shader/wgsl/SpirvEmitter.h"
#include "donner/gpu/shader/wgsl/TextEmitter.h"

namespace donner::gpu::shader::wgsl {

/// Owning structural string type for a shader source template argument.
template <size_t N>
struct SourceText {
  std::array<char, N> bytes{};
  /// Copies an inline source literal. @param text WGSL source, including its terminal NUL.
  constexpr SourceText(const char (&text)[N]) {
    for (size_t i = 0; i < N; ++i) bytes[i] = text[i];
  }
  /// Borrows the source bytes, excluding the terminal NUL.
  constexpr std::string_view view() const UTILS_LIFETIME_BOUND { return {bytes.data(), N - 1}; }
};

/// Exact-sized owned shader bytes and the interface derived from them.
template <size_t WgslBytes, size_t MslBytes, size_t SpirvWords, size_t Resources, size_t Members,
          size_t Entries, size_t InterfaceVariables>
struct CompiledShader {
  std::array<char, WgslBytes> wgsl{};
  std::array<char, MslBytes> msl{};
  std::array<uint32_t, SpirvWords> spirv{};
  std::array<ShaderResource, Resources> resources{};
  std::array<ShaderBufferMember, Members> members{};
  std::array<ShaderEntryPoint, Entries> entryPoints{};
  std::array<ShaderInterfaceVariable, InterfaceVariables> interfaceVariables{};

  /// Borrows views from this artifact. Keep the artifact alive while the views are in use.
  constexpr CompiledShaderView view() const& UTILS_LIFETIME_BOUND {
    return {{wgsl.data(), wgsl.size()},
            {msl.data(), msl.size()},
            spirv,
            resources,
            members,
            entryPoints,
            interfaceVariables};
  }
  CompiledShaderView view() const&& = delete;
};

namespace compiler_detail {

template <ErrorCode Code, uint32_t Begin, uint32_t End>
consteval bool RequireValidSource() {
  static_assert(Code == ErrorCode::None,
                "WGSL validation failed; template arguments identify the diagnostic and byte span");
  return Code == ErrorCode::None;
}

struct Emitted {
  std::array<char, ModuleLimits::kMaxSourceBytes> wgsl{};
  std::array<char, kMaxTextEmitBytes> msl{};
  std::array<uint32_t, kMaxSpirvEmitWords> spirv{};
  uint32_t wgslSize = 0;
  uint32_t mslSize = 0;
  uint32_t spirvSize = 0;
  TextEmitError wgslError = TextEmitError::None;
  TextEmitError textError = TextEmitError::None;
  SpirvEmitError binaryError = SpirvEmitError::None;
};

template <Projection Target>
constexpr Emitted Emit(const Module& module) {
  Emitted output;
  if constexpr ((uint8_t(Target) & uint8_t(Projection::Wgsl)) != 0) {
    TextSink sink{output.wgsl.data(), uint32_t(output.wgsl.size())};
    output.wgslError = EmitWgsl(module, sink).error;
    output.wgslSize = sink.size;
  }
  if constexpr ((uint8_t(Target) & uint8_t(Projection::Msl)) != 0) {
    TextSink sink{output.msl.data(), uint32_t(output.msl.size())};
    output.textError = EmitMsl(module, sink).error;
    output.mslSize = sink.size;
  }
  if constexpr ((uint8_t(Target) & uint8_t(Projection::Spirv)) != 0) {
    SpirvSink sink{output.spirv.data(), uint32_t(output.spirv.size())};
    output.binaryError = EmitSpirv(module, sink).error;
    output.spirvSize = sink.size;
  }
  return output;
}

template <SourceText Source>
inline constexpr auto kParsedSource = Parse(Source.view());

template <SourceText Source, Projection Target>
inline constexpr auto kEmittedSource = Emit<Target>(kParsedSource<Source>.module);

constexpr bool FitsNames(const Module& module) {
  for (uint16_t i = 0; i < module.bindingCount; ++i)
    if (module.bindings[i].name.length > 64) return false;
  for (uint16_t i = 0; i < module.structMemberCount; ++i)
    if (module.structMembers[i].name.length > 64) return false;
  for (uint16_t i = 0; i < module.functionCount; ++i)
    if (module.functions[i].name.length > 64) return false;
  for (uint16_t i = 0; i < module.interfaceVariableCount; ++i)
    if (module.interfaceVariables[i].name.length > 64) return false;
  return true;
}

constexpr ShaderName Name(std::string_view text) {
  ShaderName result;
  if (text.size() > result.bytes.size()) return result;
  for (size_t i = 0; i < text.size(); ++i) result.bytes[i] = text[i];
  result.size = static_cast<uint8_t>(text.size());
  return result;
}

constexpr uint16_t EntryCount(const Module& module) {
  uint16_t count = 0;
  for (uint16_t i = 0; i < module.functionCount; ++i)
    if (module.functions[i].stage != Stage::None) ++count;
  return count;
}

constexpr ShaderScalarType ScalarType(Type type) {
  if (type.kind == TypeKind::F32 || type.kind == TypeKind::Matrix) return ShaderScalarType::F32;
  if (type.kind == TypeKind::Struct) return ShaderScalarType::None;
  return type.kind == TypeKind::I32 ? ShaderScalarType::I32 : ShaderScalarType::U32;
}

constexpr ShaderBufferMember ReflectMember(const Module& module, const StructMember& member) {
  const Type value = member.type.kind == TypeKind::Array ? member.type.elementType() : member.type;
  const bool matrix = value.kind == TypeKind::Matrix;
  return {Name(module.name(member.name)),
          ScalarType(value),
          value.kind == TypeKind::Struct ? uint8_t(0)
          : matrix                       ? value.rows
                                         : value.lanes,
          member.offset,
          member.size,
          member.alignment,
          member.type.arrayCount,
          member.arrayStride,
          matrix ? value.columns : uint8_t(0),
          matrix ? (value.rows == 2 ? 8u : 16u) : 0u,
          value.kind == TypeKind::Struct ? module.structs[value.structId].firstMember : 0u,
          value.kind == TypeKind::Struct ? module.structs[value.structId].memberCount : 0u};
}

constexpr BindingType ResourceType(const Binding& binding) {
  switch (binding.kind) {
    case BindingKind::Sampler: return BindingType::FilteringSampler;
    case BindingKind::Uniform: return BindingType::UniformBuffer;
    case BindingKind::ReadOnlyStorage: return BindingType::ReadOnlyStorageBuffer;
    case BindingKind::SampledTexture:
      return binding.sampled ? BindingType::SampledTexture2dFloat
                             : BindingType::SampledTexture2dUnfilterableFloat;
    default: return BindingType::WriteOnlyStorageTexture2d;
  }
}

constexpr void ReflectBufferLayout(const Module& module, Type type, ShaderResource& resource) {
  Type layoutType = type;
  if (type.kind == TypeKind::Array) {
    resource.runtimeArrayStrideBytes = module.arrayStride(type);
    resource.minSizeBytes = resource.runtimeArrayStrideBytes;
    resource.alignmentBytes = module.typeAlignment(type);
    layoutType = type.elementType();
    if (layoutType.isNumeric()) {
      resource.runtimeArrayScalarType = ScalarType(layoutType);
      resource.runtimeArrayLanes = layoutType.lanes;
    }
  } else {
    resource.minSizeBytes = module.typeSize(type);
    resource.alignmentBytes = module.typeAlignment(type);
  }
  if (layoutType.kind == TypeKind::Struct) {
    const Struct& structure = module.structs[layoutType.structId];
    resource.firstMember = structure.firstMember;
    resource.memberCount = structure.memberCount;
  }
}

constexpr ShaderResource ReflectResource(const Module& module, const Binding& binding) {
  ShaderResource resource;
  resource.name = Name(module.name(binding.name));
  resource.group = binding.group;
  resource.binding = binding.binding;
  resource.type = ResourceType(binding);
  if (binding.kind == BindingKind::StorageTexture)
    resource.storageFormat = binding.type.storageFormat == StorageTextureFormat::Rgba8Unorm
                                 ? TextureFormat::RGBA8Unorm
                                 : TextureFormat::RGBA32Float;
  if (binding.kind == BindingKind::Uniform || binding.kind == BindingKind::ReadOnlyStorage)
    ReflectBufferLayout(module, binding.type, resource);
  return resource;
}

constexpr ShaderEntryPoint ReflectEntry(const Module& module, const Function& function) {
  return {Name(module.name(function.name)),
          function.stage == Stage::Compute  ? ShaderStage::Compute
          : function.stage == Stage::Vertex ? ShaderStage::Vertex
                                            : ShaderStage::Fragment,
          function.workgroupSize,
          function.firstInput,
          function.inputCount,
          function.firstOutput,
          function.outputCount,
          function.resourceMask};
}

constexpr ShaderBuiltin InterfaceBuiltin(BuiltinValue builtin) {
  switch (builtin) {
    case BuiltinValue::GlobalInvocationId: return ShaderBuiltin::GlobalInvocationId;
    case BuiltinValue::VertexIndex: return ShaderBuiltin::VertexIndex;
    case BuiltinValue::InstanceIndex: return ShaderBuiltin::InstanceIndex;
    case BuiltinValue::Position: return ShaderBuiltin::Position;
    default: return ShaderBuiltin::None;
  }
}

constexpr ShaderInterfaceVariable ReflectInterface(const Module& module,
                                                   const InterfaceVariable& variable) {
  return {Name(module.name(variable.name)),
          ScalarType(variable.type),
          variable.type.lanes,
          InterfaceBuiltin(variable.decoration.builtin),
          variable.decoration.location,
          variable.decoration.flat};
}

template <typename Artifact>
constexpr void FreezeInterface(const Module& module, Artifact& result) {
  for (uint16_t i = 0; i < module.structMemberCount; ++i)
    result.members[i] = ReflectMember(module, module.structMembers[i]);
  for (uint16_t i = 0; i < module.bindingCount; ++i)
    result.resources[i] = ReflectResource(module, module.bindings[i]);
  size_t entry = 0;
  for (uint16_t i = 0; i < module.functionCount; ++i) {
    const Function& function = module.functions[i];
    if (function.stage != Stage::None) result.entryPoints[entry++] = ReflectEntry(module, function);
  }
  for (uint16_t i = 0; i < module.interfaceVariableCount; ++i)
    result.interfaceVariables[i] = ReflectInterface(module, module.interfaceVariables[i]);
}

}  // namespace compiler_detail

/// Compiles authored source immediately, failing C++ compilation on validation/emission failure.
/// @tparam Source Inline WGSL source. @tparam Target Projections retained in the artifact.
template <SourceText Source, Projection Target>
consteval auto Compile() {
  static_assert(uint8_t(Target) != 0 && (uint8_t(Target) & ~uint8_t(Projection::All)) == 0,
                "Unknown WGSL projection selection");
  constexpr const auto& parsed = compiler_detail::kParsedSource<Source>;
  static_assert(
      compiler_detail::RequireValidSource<parsed.diagnostic.code, parsed.diagnostic.span.begin,
                                          parsed.diagnostic.span.end>());
  static_assert(parsed.hasResult(),
                "WGSL parsing or validation failed; inspect diagnostic code and span");
  static_assert(compiler_detail::EntryCount(parsed.module) > 0,
                "The compiled artifact requires at least one entry point");
  static_assert(compiler_detail::FitsNames(parsed.module),
                "WGSL interface name exceeds artifact limit");
  constexpr const auto& emitted = compiler_detail::kEmittedSource<Source, Target>;
  static_assert(emitted.wgslError == TextEmitError::None, "WGSL projection failed");
  static_assert(emitted.textError == TextEmitError::None, "WGSL text projection failed");
  static_assert(emitted.binaryError == SpirvEmitError::None, "WGSL SPIR-V projection failed");
  CompiledShader<emitted.wgslSize, emitted.mslSize, emitted.spirvSize, parsed.module.bindingCount,
                 parsed.module.structMemberCount, compiler_detail::EntryCount(parsed.module),
                 parsed.module.interfaceVariableCount>
      result;
  for (size_t i = 0; i < emitted.wgslSize; ++i) result.wgsl[i] = emitted.wgsl[i];
  for (size_t i = 0; i < emitted.mslSize; ++i) result.msl[i] = emitted.msl[i];
  for (size_t i = 0; i < emitted.spirvSize; ++i) result.spirv[i] = emitted.spirv[i];
  compiler_detail::FreezeInterface(parsed.module, result);
  return result;
}

}  // namespace donner::gpu::shader::wgsl
