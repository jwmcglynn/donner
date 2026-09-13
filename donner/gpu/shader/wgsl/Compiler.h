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
  std::array<char, 32768> msl{};
  std::array<uint32_t, 24576> spirv{};
  uint32_t mslSize = 0;
  uint32_t spirvSize = 0;
  TextEmitError textError = TextEmitError::None;
  SpirvEmitError binaryError = SpirvEmitError::None;
};

template <Projection Target>
constexpr Emitted Emit(const Module& module) {
  Emitted output;
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

}  // namespace compiler_detail

/// Compiles authored source immediately, failing C++ compilation on validation/emission failure.
/// @tparam Source Inline WGSL source. @tparam Target Projections retained in the artifact.
template <SourceText Source, Projection Target>
consteval auto Compile() {
  static_assert(uint8_t(Target) != 0 && (uint8_t(Target) & ~uint8_t(Projection::All)) == 0,
                "Unknown WGSL projection selection");
  constexpr auto parsed = Parse(Source.view());
  static_assert(
      compiler_detail::RequireValidSource<parsed.diagnostic.code, parsed.diagnostic.span.begin,
                                          parsed.diagnostic.span.end>());
  static_assert(parsed.hasResult(),
                "WGSL parsing or validation failed; inspect diagnostic code and span");
  static_assert(compiler_detail::EntryCount(parsed.module) > 0,
                "The compiled artifact requires at least one entry point");
  static_assert(compiler_detail::FitsNames(parsed.module),
                "WGSL interface name exceeds artifact limit");
  constexpr auto emitted = compiler_detail::Emit<Target>(parsed.module);
  static_assert(emitted.textError == TextEmitError::None, "WGSL text projection failed");
  static_assert(emitted.binaryError == SpirvEmitError::None, "WGSL SPIR-V projection failed");
  constexpr size_t wgslBytes =
      (uint8_t(Target) & uint8_t(Projection::Wgsl)) ? Source.view().size() : 0;
  CompiledShader<wgslBytes, emitted.mslSize, emitted.spirvSize, parsed.module.bindingCount,
                 parsed.module.structMemberCount, compiler_detail::EntryCount(parsed.module),
                 parsed.module.interfaceVariableCount>
      result;
  for (size_t i = 0; i < wgslBytes; ++i) result.wgsl[i] = Source.bytes[i];
  for (size_t i = 0; i < emitted.mslSize; ++i) result.msl[i] = emitted.msl[i];
  for (size_t i = 0; i < emitted.spirvSize; ++i) result.spirv[i] = emitted.spirv[i];
  for (size_t i = 0; i < parsed.module.structMemberCount; ++i) {
    const StructMember& member = parsed.module.structMembers[i];
    const Type valueType =
        member.type.kind == TypeKind::Array ? member.type.elementType() : member.type;
    result.members[i] = {
        compiler_detail::Name(parsed.module.name(member.name)),
        valueType.kind == TypeKind::F32 || valueType.kind == TypeKind::Matrix
            ? ShaderScalarType::F32
        : valueType.kind == TypeKind::I32 ? ShaderScalarType::I32
                                          : ShaderScalarType::U32,
        valueType.kind == TypeKind::Matrix ? valueType.rows : valueType.lanes,
        member.offset,
        member.size,
        member.alignment,
        member.type.arrayCount,
        member.arrayStride,
        valueType.kind == TypeKind::Matrix ? valueType.columns : uint8_t(0),
        valueType.kind == TypeKind::Matrix ? (valueType.rows == 2 ? 8u : 16u) : 0u};
  }
  for (size_t i = 0; i < parsed.module.bindingCount; ++i) {
    const Binding& binding = parsed.module.bindings[i];
    ShaderResource& resource = result.resources[i];
    resource.name = compiler_detail::Name(parsed.module.name(binding.name));
    resource.group = binding.group;
    resource.binding = binding.binding;
    resource.type = binding.kind == BindingKind::Uniform ? BindingType::UniformBuffer
                    : binding.kind == BindingKind::ReadOnlyStorage
                        ? BindingType::ReadOnlyStorageBuffer
                    : binding.kind == BindingKind::SampledTexture
                        ? BindingType::SampledTexture2dUnfilterableFloat
                        : BindingType::WriteOnlyStorageTexture2d;
    if (binding.kind == BindingKind::Uniform || binding.kind == BindingKind::ReadOnlyStorage) {
      Type layoutType = binding.type;
      if (binding.type.kind == TypeKind::Array) {
        resource.runtimeArrayStrideBytes = parsed.module.arrayStride(binding.type);
        resource.minSizeBytes = resource.runtimeArrayStrideBytes;
        resource.alignmentBytes = parsed.module.typeAlignment(binding.type);
        layoutType = binding.type.elementType();
      } else {
        resource.minSizeBytes = parsed.module.typeSize(binding.type);
        resource.alignmentBytes = parsed.module.typeAlignment(binding.type);
      }
      if (layoutType.kind == TypeKind::Struct) {
        const Struct& structure = parsed.module.structs[layoutType.structId];
        resource.firstMember = structure.firstMember;
        resource.memberCount = structure.memberCount;
      }
    }
  }

  size_t entryIndex = 0;
  for (uint16_t i = 0; i < parsed.module.functionCount; ++i) {
    const Function& function = parsed.module.functions[i];
    if (function.stage == Stage::None) continue;
    result.entryPoints[entryIndex++] = {compiler_detail::Name(parsed.module.name(function.name)),
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
  for (uint16_t i = 0; i < parsed.module.interfaceVariableCount; ++i) {
    const InterfaceVariable& variable = parsed.module.interfaceVariables[i];
    result.interfaceVariables[i] = {
        compiler_detail::Name(parsed.module.name(variable.name)),
        variable.type.kind == TypeKind::F32   ? ShaderScalarType::F32
        : variable.type.kind == TypeKind::I32 ? ShaderScalarType::I32
                                              : ShaderScalarType::U32,
        variable.type.lanes,
        variable.decoration.builtin == BuiltinValue::GlobalInvocationId
            ? ShaderBuiltin::GlobalInvocationId
        : variable.decoration.builtin == BuiltinValue::VertexIndex ? ShaderBuiltin::VertexIndex
        : variable.decoration.builtin == BuiltinValue::Position    ? ShaderBuiltin::Position
                                                                   : ShaderBuiltin::None,
        variable.decoration.location};
  }
  return result;
}

}  // namespace donner::gpu::shader::wgsl
