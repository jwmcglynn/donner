#pragma once
/// @file
/// Immutable type model for the \c donner::gpu::shader IR.
///
/// The type surface covers exactly what the solid-fill pipeline family requires (design 0053
/// "Donner shader IR"): scalars, vectors, mat4x4f, sized and runtime arrays, structs, and the
/// texture_2d<f32> / sampler resource types. Types are immutable values with deep equality, so
/// identical types compare equal deterministically regardless of construction order.

#include <cstdint>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/gpu/shader/ShaderResult.h"

namespace donner::gpu::shader {

/// Scalar component types.
enum class ScalarKind : uint8_t {
  Bool,  //!< `bool`. Not host-shareable; no layout.
  I32,   //!< 32-bit signed integer.
  U32,   //!< 32-bit unsigned integer.
  F32,   //!< 32-bit float.
};

/**
 * Ostream output operator, e.g. `f32`.
 *
 * @param os Output stream.
 * @param value Value to output.
 */
std::ostream& operator<<(std::ostream& os, ScalarKind value);

/**
 * An immutable shader IR type with deep value equality.
 *
 * Construct through the static factories. Factories that can receive invalid input (arrays,
 * structs) return \ref ShaderResult and fail closed; the fixed factories (scalars, vectors,
 * matrix, resource types) cannot fail.
 */
class IrType {
public:
  /// Type category.
  enum class Kind : uint8_t {
    Scalar,        //!< bool / i32 / u32 / f32.
    Vector,        //!< vecN<T>, N in 2..4.
    Matrix4x4f,    //!< mat4x4f (the only matrix type in scope).
    SizedArray,    //!< array<E, N>.
    RuntimeArray,  //!< array<E>; storage buffer roots only.
    Struct,        //!< Named struct with named members.
    Texture2dF32,  //!< texture_2d<f32>.
    Sampler,       //!< Filtering sampler.
  };

  /// One named member of a struct type; defined after the class (members hold IrType by value).
  struct Member;

  /**
   * Scalar type of the given kind.
   *
   * @param kind Scalar kind.
   */
  static IrType Scalar(ScalarKind kind);

  /// `bool` type.
  static IrType Bool();
  /// `i32` type.
  static IrType I32();
  /// `u32` type.
  static IrType U32();
  /// `f32` type.
  static IrType F32();

  /**
   * `vec2<element>` type.
   *
   * @param element Component scalar kind.
   */
  static IrType Vec2(ScalarKind element);
  /**
   * `vec3<element>` type.
   *
   * @param element Component scalar kind.
   */
  static IrType Vec3(ScalarKind element);
  /**
   * `vec4<element>` type.
   *
   * @param element Component scalar kind.
   */
  static IrType Vec4(ScalarKind element);

  /// `vec2<f32>` convenience factory.
  static IrType Vec2f() { return Vec2(ScalarKind::F32); }
  /// `vec3<f32>` convenience factory.
  static IrType Vec3f() { return Vec3(ScalarKind::F32); }
  /// `vec4<f32>` convenience factory.
  static IrType Vec4f() { return Vec4(ScalarKind::F32); }
  /// `vec2<i32>` convenience factory.
  static IrType Vec2i() { return Vec2(ScalarKind::I32); }
  /// `vec2<u32>` convenience factory.
  static IrType Vec2u() { return Vec2(ScalarKind::U32); }

  /// `mat4x4<f32>` type.
  static IrType Mat4x4f();

  /// `texture_2d<f32>` resource type.
  static IrType Texture2dF32();
  /// Filtering sampler resource type.
  static IrType SamplerType();

  /**
   * `array<element, count>` type. Fails closed if \p count is zero or \p element is not plain
   * data (runtime arrays, textures, and samplers cannot be array elements).
   *
   * @param element Element type.
   * @param count Element count; must be nonzero.
   * @param label Diagnostic label for errors.
   */
  static ShaderResult<IrType> SizedArray(const IrType& element, uint32_t count,
                                         const RcString& label = "array");

  /**
   * `array<element>` runtime-sized type, usable only as a read-only storage buffer root. Fails
   * closed if \p element is not plain data.
   *
   * @param element Element type.
   * @param label Diagnostic label for errors.
   */
  static ShaderResult<IrType> RuntimeArray(const IrType& element,
                                           const RcString& label = "runtimeArray");

  /**
   * Named struct type. Fails closed on empty member lists, duplicate member names, or member
   * types that are not plain data (runtime arrays are only supported as storage binding roots in
   * this packet, so they cannot be struct members).
   *
   * @param name Struct name.
   * @param members Struct members in declaration order.
   * @param label Diagnostic label for errors.
   */
  static ShaderResult<IrType> Struct(const RcString& name, std::vector<Member> members,
                                     const RcString& label = "struct");

  /// Type category.
  Kind kind() const;

  /// Scalar kind of a scalar or vector type. @pre `kind()` is Scalar or Vector.
  ScalarKind scalarKind() const;

  /// Component count of a vector type (2..4). @pre `kind()` is Vector.
  uint32_t vectorSize() const;

  /// Element type of a sized or runtime array. @pre `kind()` is SizedArray or RuntimeArray.
  const IrType& elementType() const;

  /// Element count of a sized array. @pre `kind()` is SizedArray.
  uint32_t arrayCount() const;

  /// Name of a struct type. @pre `kind()` is Struct.
  const RcString& structName() const;

  /// Members of a struct type. @pre `kind()` is Struct.
  std::span<const Member> structMembers() const;

  /// True for bool/i32/u32/f32.
  bool isScalar() const { return kind() == Kind::Scalar; }
  /// True for vecN types.
  bool isVector() const { return kind() == Kind::Vector; }
  /// True for i32/u32/f32 scalars.
  bool isNumericScalar() const;
  /// True for vectors of i32/u32/f32.
  bool isNumericVector() const;
  /// True for i32/u32/f32 scalars and their vectors.
  bool isNumeric() const { return isNumericScalar() || isNumericVector(); }
  /// True for `f32` and vectors of f32.
  bool isFloatScalarOrVector() const;
  /// True for types that can be stored in arrays and structs (everything except runtime arrays,
  /// textures, and samplers).
  bool isPlainData() const;

  /**
   * Deep structural equality.
   *
   * @param other Type to compare against.
   */
  bool operator==(const IrType& other) const;

  /// Formats this type, e.g. `vec2<f32>` or `array<Band>`.
  std::string toString() const;

  /**
   * Ostream output operator.
   *
   * @param os Output stream.
   * @param type Type to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const IrType& type) {
    return os << type.toString();
  }

private:
  struct Node;

  explicit IrType(std::shared_ptr<const Node> node) : node_(std::move(node)) {}

  /// Builds a scalar type node. @param kind Scalar kind.
  static IrType MakeScalarType(ScalarKind kind);
  /// Builds a vector type node. @param kind Component kind. @param size Component count.
  static IrType MakeVectorType(ScalarKind kind, uint32_t size);
  /// Builds a node for a childless type category. @param kind Type category.
  static IrType MakeSimpleType(Kind kind);

  std::shared_ptr<const Node> node_;
};

/// One named member of a struct type.
struct IrType::Member {
  RcString name;  //!< Member name; unique within the struct.
  IrType type;    //!< Member type.
};

}  // namespace donner::gpu::shader
