#pragma once
/// @file
/// Immutable shader artifacts and reflected resource interfaces, independent of compiler code.

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/gpu/Descriptors.h"

namespace donner::gpu::shader {

/// A small owning identifier in a frozen shader interface.
struct ShaderName {
  std::array<char, 64> bytes{};
  uint8_t size = 0;

  /// Borrows this identifier's bytes.
  constexpr std::string_view view() const UTILS_LIFETIME_BOUND { return {bytes.data(), size}; }
};

/// Numeric component format of a buffer member.
enum class ShaderScalarType : uint8_t { I32, U32, F32 };

/// A reflected buffer member, including the layout required by the shader.
struct ShaderBufferMember {
  ShaderName name;
  ShaderScalarType scalarType = ShaderScalarType::F32;
  uint8_t lanes = 1;
  uint32_t offsetBytes = 0;
  uint32_t sizeBytes = 0;
  uint32_t alignmentBytes = 1;
  uint32_t arrayCount = 0;        //!< Fixed array length, or zero for a scalar/vector member.
  uint32_t arrayStrideBytes = 0;  //!< Byte stride for a fixed array member.
};

/// One resource and the layout of its parameter block, when applicable.
struct ShaderResource {
  ShaderName name;
  BindingType type = BindingType::UniformBuffer;
  uint32_t group = 0;
  uint32_t binding = 0;
  uint32_t minSizeBytes = 0;
  uint32_t alignmentBytes = 1;
  uint32_t firstMember = 0;
  uint32_t memberCount = 0;
  TextureFormat storageFormat = TextureFormat::RGBA32Float;
};

/// Builtin reflected by an entry-point interface.
enum class ShaderBuiltin : uint8_t { None, GlobalInvocationId, VertexIndex, Position };

/// A flattened scalar/vector entry-point input or output.
struct ShaderInterfaceVariable {
  ShaderName name;
  ShaderScalarType scalarType = ShaderScalarType::F32;
  uint8_t lanes = 1;
  ShaderBuiltin builtin = ShaderBuiltin::None;
  uint32_t location = UINT32_MAX;
};

/// One entry point and its source-derived interface ranges.
struct ShaderEntryPoint {
  ShaderName name;
  ShaderStage stage = ShaderStage::None;              //!< Exactly one shader stage.
  std::array<uint32_t, 3> workgroupSize = {1, 1, 1};  //!< Compute workgroup dimensions only.
  uint32_t firstInput = 0;    //!< First input in the artifact interface-variable array.
  uint32_t inputCount = 0;    //!< Number of flattened inputs.
  uint32_t firstOutput = 0;   //!< First output in the artifact interface-variable array.
  uint32_t outputCount = 0;   //!< Number of flattened outputs.
  uint32_t resourceMask = 0;  //!< Bit i identifies an accessed resource in the artifact.
};

/// Borrowed views into a shader artifact with static storage in the owning program.
struct CompiledShaderView {
  std::string_view wgsl;
  std::string_view msl;
  std::span<const uint32_t> spirv;
  std::span<const ShaderResource> resources;
  std::span<const ShaderBufferMember> members;
  std::span<const ShaderEntryPoint> entryPoints;
  std::span<const ShaderInterfaceVariable> interfaceVariables;

  /// Finds a resource by its authored WGSL name; the result borrows this view's artifact.
  /// @param name Authored resource name.
  constexpr const ShaderResource* resource(std::string_view name) const UTILS_LIFETIME_BOUND {
    for (const auto& value : resources)
      if (value.name.view() == name) return &value;
    return nullptr;
  }

  /// Checks one C++ field against the corresponding shader member.
  /// @param resourceName Buffer resource name. @param memberName Member name.
  /// @param offsetBytes C++ field offset. @param sizeBytes C++ field size.
  /// @param scalarType C++ numeric component kind. @param lanes C++ component count.
  /// @param arrayCount Fixed array length, or zero for a non-array member.
  /// @param arrayStrideBytes Fixed array element stride, or zero for a non-array member.
  constexpr bool matchesMember(std::string_view resourceName, std::string_view memberName,
                               uint32_t offsetBytes, uint32_t sizeBytes,
                               ShaderScalarType scalarType, uint8_t lanes = 1,
                               uint32_t arrayCount = 0, uint32_t arrayStrideBytes = 0) const {
    const ShaderResource* binding = resource(resourceName);
    if (!binding || binding->firstMember > members.size() ||
        binding->memberCount > members.size() - binding->firstMember)
      return false;
    for (uint32_t index = binding->firstMember; index < binding->firstMember + binding->memberCount;
         ++index) {
      const ShaderBufferMember& member = members[index];
      if (member.name.view() == memberName)
        return member.offsetBytes == offsetBytes && member.sizeBytes == sizeBytes &&
               member.scalarType == scalarType && member.lanes == lanes &&
               member.arrayCount == arrayCount && member.arrayStrideBytes == arrayStrideBytes;
    }
    return false;
  }
};

/// Builds a runtime descriptor from immutable precompiled data. No shader compiler is invoked.
/// @param shader Static artifact views. @param kind Device-selected projection.
/// @param label Diagnostic shader label.
ShaderModuleDescriptor MakeShaderDescriptor(const CompiledShaderView& shader, ShaderSourceKind kind,
                                            std::string_view label);

/// Derives a group-zero layout and stage visibility from statically accessed resources.
/// @param shader Static artifact views.
std::vector<BindGroupLayoutEntry> MakeBindingLayout(const CompiledShaderView& shader);

}  // namespace donner::gpu::shader
