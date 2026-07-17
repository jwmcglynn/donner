#include "donner/gpu/shader/IrLayout.h"

#include <algorithm>
#include <format>

namespace donner::gpu::shader {

namespace {

/// Rounds \p value up to the next multiple of \p alignment.
uint32_t RoundUp(uint32_t alignment, uint32_t value) {
  return ((value + alignment - 1) / alignment) * alignment;
}

/// Alignment of \p type as a member or array element in \p addressSpace: in uniform space,
/// structs and arrays round their alignment up to 16 (WGSL uniform layout constraints).
uint32_t EffectiveAlign(const IrType& type, AddressSpace addressSpace, uint32_t baseAlign) {
  if (addressSpace == AddressSpace::Uniform &&
      (type.kind() == IrType::Kind::Struct || type.kind() == IrType::Kind::SizedArray)) {
    return RoundUp(16, baseAlign);
  }
  return baseAlign;
}

}  // namespace

std::ostream& operator<<(std::ostream& os, AddressSpace value) {
  switch (value) {
    case AddressSpace::Uniform: return os << "uniform";
    case AddressSpace::Storage: return os << "storage";
  }
  return os << "unknown";
}

ShaderResult<TypeLayout> ComputeTypeLayout(const IrType& type, AddressSpace addressSpace) {
  switch (type.kind()) {
    case IrType::Kind::Scalar:
      if (type.scalarKind() == ScalarKind::Bool) {
        return ShaderError{"bool is not host-shareable and has no layout", "layout"};
      }
      return TypeLayout{4, 4};

    case IrType::Kind::Vector: {
      if (type.scalarKind() == ScalarKind::Bool) {
        return ShaderError{"vectors of bool are not host-shareable and have no layout", "layout"};
      }
      switch (type.vectorSize()) {
        case 2: return TypeLayout{8, 8};
        case 3: return TypeLayout{16, 12};
        case 4: return TypeLayout{16, 16};
        default: break;
      }
      return ShaderError{std::format("vector size {} has no layout", type.vectorSize()), "layout"};
    }

    case IrType::Kind::Matrix4x4f: return TypeLayout{16, 64};

    case IrType::Kind::SizedArray: {
      ShaderResult<TypeLayout> elementLayout = ComputeTypeLayout(type.elementType(), addressSpace);
      if (elementLayout.hasError()) {
        return std::move(elementLayout).error();
      }
      ShaderResult<uint32_t> stride = ComputeArrayStride(type, addressSpace);
      if (stride.hasError()) {
        return std::move(stride).error();
      }
      // WGSL: in the uniform address space, arrays align to roundUp(16, AlignOf(element)).
      uint32_t align = elementLayout.result().alignBytes;
      if (addressSpace == AddressSpace::Uniform) {
        align = RoundUp(16, align);
      }
      return TypeLayout{align, type.arrayCount() * stride.result()};
    }

    case IrType::Kind::RuntimeArray:
      return ShaderError{"runtime arrays have a stride but no fixed size; use ComputeArrayStride",
                         "layout"};

    case IrType::Kind::Struct: {
      ShaderResult<StructLayout> structLayout = ComputeStructLayout(type, addressSpace);
      if (structLayout.hasError()) {
        return std::move(structLayout).error();
      }
      return TypeLayout{structLayout.result().alignBytes, structLayout.result().sizeBytes};
    }

    case IrType::Kind::Texture2dF32:
    case IrType::Kind::Sampler:
      return ShaderError{
          std::format("{} is a resource type and has no host-shareable layout", type.toString()),
          "layout"};
  }

  return ShaderError{"unknown type kind", "layout"};
}

ShaderResult<uint32_t> ComputeArrayStride(const IrType& arrayType, AddressSpace addressSpace) {
  if (arrayType.kind() != IrType::Kind::SizedArray &&
      arrayType.kind() != IrType::Kind::RuntimeArray) {
    return ShaderError{std::format("{} is not an array type", arrayType.toString()), "arrayStride"};
  }

  ShaderResult<TypeLayout> elementLayout = ComputeTypeLayout(arrayType.elementType(), addressSpace);
  if (elementLayout.hasError()) {
    return std::move(elementLayout).error();
  }

  uint32_t stride = RoundUp(elementLayout.result().alignBytes, elementLayout.result().sizeBytes);
  if (addressSpace == AddressSpace::Uniform) {
    // WGSL uniform address space requires array element strides to be multiples of 16.
    stride = RoundUp(16, stride);
  }
  return stride;
}

ShaderResult<StructLayout> ComputeStructLayout(const IrType& structType,
                                               AddressSpace addressSpace) {
  if (structType.kind() != IrType::Kind::Struct) {
    return ShaderError{std::format("{} is not a struct type", structType.toString()),
                       "structLayout"};
  }

  StructLayout layout;
  uint32_t offset = 0;
  for (const IrType::Member& member : structType.structMembers()) {
    ShaderResult<TypeLayout> memberLayout = ComputeTypeLayout(member.type, addressSpace);
    if (memberLayout.hasError()) {
      ShaderError error = std::move(memberLayout).error();
      error.message = std::format("struct {} member {}: {}", structType.structName().str(),
                                  member.name.str(), error.message);
      return error;
    }

    const uint32_t memberAlign =
        EffectiveAlign(member.type, addressSpace, memberLayout.result().alignBytes);
    offset = RoundUp(memberAlign, offset);
    layout.members.push_back(
        StructMemberLayout{offset, TypeLayout{memberAlign, memberLayout.result().sizeBytes}});
    layout.alignBytes = std::max(layout.alignBytes, memberAlign);

    // WGSL uniform constraint: the member following a struct-typed member S must start at least
    // roundUp(16, SizeOf(S)) bytes after S's start, even when the follower's own alignment would
    // let it pack into S's tail padding.
    if (addressSpace == AddressSpace::Uniform && member.type.kind() == IrType::Kind::Struct) {
      offset += RoundUp(16, memberLayout.result().sizeBytes);
    } else {
      offset += memberLayout.result().sizeBytes;
    }
  }

  layout.sizeBytes = RoundUp(layout.alignBytes, offset);
  return layout;
}

}  // namespace donner::gpu::shader
