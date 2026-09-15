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

/// Numeric component format; None denotes a structure-valued member.
enum class ShaderScalarType : uint8_t { I32, U32, F32, None };

/// A reflected buffer member, including the layout required by the shader.
struct ShaderBufferMember {
  ShaderName name;
  ShaderScalarType scalarType = ShaderScalarType::F32;
  uint8_t lanes = 1;
  uint32_t offsetBytes = 0;
  uint32_t sizeBytes = 0;
  uint32_t alignmentBytes = 1;
  uint32_t arrayCount = 0;         //!< Fixed array length, or zero for a scalar/vector member.
  uint32_t arrayStrideBytes = 0;   //!< Byte stride for a fixed array member.
  uint8_t matrixColumns = 0;       //!< Matrix columns; lanes gives rows. Zero for non-matrices.
  uint32_t matrixStrideBytes = 0;  //!< Byte stride between matrix columns.
  uint32_t firstMember = 0;        //!< Child range for a structure or structure array element.
  uint32_t memberCount = 0;        //!< Zero for scalar/vector/matrix members.
};

/// One resource and the layout of its parameter block, when applicable.
struct ShaderResource {
  ShaderName name;
  BindingType type = BindingType::UniformBuffer;
  uint32_t group = 0;
  uint32_t binding = 0;
  uint32_t minSizeBytes = 0;
  uint32_t alignmentBytes = 1;
  uint32_t runtimeArrayStrideBytes = 0;  //!< Zero for fixed-size resources.
  ShaderScalarType runtimeArrayScalarType = ShaderScalarType::F32;
  uint8_t runtimeArrayLanes = 0;  //!< Zero for non-arrays or structure elements.
  uint32_t firstMember = 0;
  uint32_t memberCount = 0;
  TextureFormat storageFormat = TextureFormat::RGBA32Float;
};

/// Builtin reflected by an entry-point interface.
enum class ShaderBuiltin : uint8_t {
  None,
  GlobalInvocationId,
  VertexIndex,
  Position,
  InstanceIndex
};

/// A flattened scalar/vector entry-point input or output.
struct ShaderInterfaceVariable {
  ShaderName name;
  ShaderScalarType scalarType = ShaderScalarType::F32;
  uint8_t lanes = 1;
  ShaderBuiltin builtin = ShaderBuiltin::None;
  uint32_t location = UINT32_MAX;
  bool flat = false;  //!< Flat interstage interpolation.
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

  /// Finds a member within a validated reflected range; the result borrows this artifact.
  /// @param first First member. @param count Member count. @param name Unqualified member name.
  constexpr const ShaderBufferMember* findMember(uint32_t first, uint32_t count,
                                                 std::string_view name) const UTILS_LIFETIME_BOUND {
    if (first > members.size() || count > members.size() - first) return nullptr;
    for (uint32_t i = first; i < first + count; ++i)
      if (members[i].name.view() == name) return &members[i];
    return nullptr;
  }

  /// Compares one resolved member with its expected absolute layout.
  static constexpr bool matchesLayout(const ShaderBufferMember& member, uint32_t absoluteOffset,
                                      uint32_t offset, uint32_t size, ShaderScalarType scalar,
                                      uint8_t lanes, uint32_t count, uint32_t stride,
                                      uint8_t columns, uint32_t matrixStride) {
    return absoluteOffset == offset && member.sizeBytes == size && member.scalarType == scalar &&
           member.lanes == lanes && member.arrayCount == count &&
           member.arrayStrideBytes == stride && member.matrixColumns == columns &&
           member.matrixStrideBytes == matrixStride;
  }

  /// Checks one C++ field against the corresponding shader member.
  /// @param resourceName Buffer resource name. @param memberName Member name or dotted structure
  /// path.
  /// @param offsetBytes C++ field offset. @param sizeBytes C++ field size.
  /// @param scalarType C++ numeric component kind. @param lanes C++ component count.
  /// @param arrayCount Fixed array length, or zero for a non-array member.
  /// @param arrayStrideBytes Fixed array element stride, or zero for a non-array member.
  /// @param matrixColumns Matrix columns, or zero for a non-matrix member.
  /// @param matrixStrideBytes Matrix column stride, or zero for a non-matrix member.
  constexpr bool matchesMember(std::string_view resourceName, std::string_view memberName,
                               uint32_t offsetBytes, uint32_t sizeBytes,
                               ShaderScalarType scalarType, uint8_t lanes = 1,
                               uint32_t arrayCount = 0, uint32_t arrayStrideBytes = 0,
                               uint8_t matrixColumns = 0, uint32_t matrixStrideBytes = 0) const {
    const ShaderResource* binding = resource(resourceName);
    if (!binding) return false;
    uint32_t first = binding->firstMember, count = binding->memberCount, baseOffset = 0;
    while (!memberName.empty()) {
      const size_t dot = memberName.find('.');
      const auto part = memberName.substr(0, dot);
      const ShaderBufferMember* member = findMember(first, count, part);
      if (!member || member->offsetBytes > UINT32_MAX - baseOffset) return false;
      baseOffset += member->offsetBytes;
      if (dot == std::string_view::npos)
        return matchesLayout(*member, baseOffset, offsetBytes, sizeBytes, scalarType, lanes,
                             arrayCount, arrayStrideBytes, matrixColumns, matrixStrideBytes);
      if (member->arrayCount != 0) return false;
      first = member->firstMember;
      count = member->memberCount;
      memberName.remove_prefix(dot + 1);
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
