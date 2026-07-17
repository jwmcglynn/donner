#pragma once
/// @file
/// Host-shareable memory layout engine for \c donner::gpu::shader types.
///
/// Implements the WGSL specification's memory layout rules: scalar align/size 4/4, vec2 8/8,
/// vec3 16/12, vec4 16/16, mat4x4f 16/64, array stride roundUp(AlignOf(elem), SizeOf(elem)),
/// struct member offsets rounded to member alignment, struct alignment = max member alignment,
/// struct size rounded up to the struct alignment. In the uniform address space, arrays round
/// their element stride up to a multiple of 16, and members whose type is a struct or array
/// round their alignment up to 16.
///
/// The C++ mirror structs the GPU runtime uploads (e.g. the Geode encoder's Uniforms/Band/
/// InstanceTransform) must byte-match these computed layouts; the shader tests anchor that.

#include <cstdint>
#include <ostream>
#include <vector>

#include "donner/gpu/shader/IrType.h"
#include "donner/gpu/shader/ShaderResult.h"

namespace donner::gpu::shader {

/// Address space a host-shareable layout is computed for.
enum class AddressSpace : uint8_t {
  Uniform,  //!< Uniform buffers; extra 16-byte rounding rules apply.
  Storage,  //!< (Read-only) storage buffers; base layout rules.
};

/**
 * Ostream output operator, e.g. `uniform`.
 *
 * @param os Output stream.
 * @param value Value to output.
 */
std::ostream& operator<<(std::ostream& os, AddressSpace value);

/// Alignment and size of a host-shareable type.
struct TypeLayout {
  uint32_t alignBytes = 0;  //!< Required alignment in bytes.
  uint32_t sizeBytes = 0;   //!< Size in bytes (for sized types).

  /// Equality operator. @param other Layout to compare against.
  bool operator==(const TypeLayout& other) const = default;

  /// Ostream output operator, e.g. `align=16 size=288`.
  /// @param os Output stream. @param value Layout to output.
  friend std::ostream& operator<<(std::ostream& os, const TypeLayout& value) {
    return os << "align=" << value.alignBytes << " size=" << value.sizeBytes;
  }
};

/// Layout of one struct member.
struct StructMemberLayout {
  uint32_t offsetBytes = 0;  //!< Byte offset from the start of the struct.
  TypeLayout layout;         //!< Member type layout in the queried address space.

  /// Equality operator. @param other Layout to compare against.
  bool operator==(const StructMemberLayout& other) const = default;
};

/// Layout of a struct type: overall alignment/size plus per-member offsets in declaration order.
struct StructLayout {
  uint32_t alignBytes = 0;                  //!< Struct alignment.
  uint32_t sizeBytes = 0;                   //!< Struct size, rounded up to the alignment.
  std::vector<StructMemberLayout> members;  //!< Per-member layouts, in declaration order.
};

/**
 * Computes alignment and size of \p type in \p addressSpace per the WGSL layout rules. Fails
 * closed for types with no host-shareable layout: bool (and vectors of bool), textures,
 * samplers, and runtime arrays (which have a stride but no fixed size; use
 * \ref ComputeArrayStride).
 *
 * @param type Type to lay out.
 * @param addressSpace Address space the layout is computed for.
 */
ShaderResult<TypeLayout> ComputeTypeLayout(const IrType& type, AddressSpace addressSpace);

/**
 * Computes the element stride of a sized or runtime array in \p addressSpace:
 * roundUp(AlignOf(element), SizeOf(element)), rounded up to a multiple of 16 in the uniform
 * address space.
 *
 * @param arrayType Sized or runtime array type.
 * @param addressSpace Address space the stride is computed for.
 */
ShaderResult<uint32_t> ComputeArrayStride(const IrType& arrayType, AddressSpace addressSpace);

/**
 * Computes the full member layout of a struct type in \p addressSpace: member offsets rounded to
 * member alignment (16-rounded for nested structs/arrays in uniform), struct alignment = max
 * member alignment, struct size rounded up to the struct alignment.
 *
 * @param structType Struct type to lay out.
 * @param addressSpace Address space the layout is computed for.
 */
ShaderResult<StructLayout> ComputeStructLayout(const IrType& structType, AddressSpace addressSpace);

}  // namespace donner::gpu::shader
