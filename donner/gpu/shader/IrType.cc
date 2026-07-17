#include "donner/gpu/shader/IrType.h"

#include <format>
#include <utility>

namespace donner::gpu::shader {

/// Internal storage for \ref IrType. One node per type; children held by value (IrType handles).
struct IrType::Node {
  Kind kind = Kind::Scalar;             //!< Type category.
  ScalarKind scalar = ScalarKind::F32;  //!< Scalar/vector component kind.
  uint32_t vectorSize = 0;              //!< Vector component count (2..4).
  std::vector<IrType> element;          //!< Array element type (0 or 1 entries).
  uint32_t arrayCount = 0;              //!< Sized array element count.
  RcString structName;                  //!< Struct name.
  std::vector<Member> members;          //!< Struct members.
};

std::ostream& operator<<(std::ostream& os, ScalarKind value) {
  switch (value) {
    case ScalarKind::Bool: return os << "bool";
    case ScalarKind::I32: return os << "i32";
    case ScalarKind::U32: return os << "u32";
    case ScalarKind::F32: return os << "f32";
  }
  return os << "unknown";
}

IrType IrType::MakeScalarType(ScalarKind kind) {
  Node node;
  node.kind = Kind::Scalar;
  node.scalar = kind;
  return IrType(std::make_shared<const Node>(std::move(node)));
}

IrType IrType::MakeVectorType(ScalarKind kind, uint32_t size) {
  Node node;
  node.kind = Kind::Vector;
  node.scalar = kind;
  node.vectorSize = size;
  return IrType(std::make_shared<const Node>(std::move(node)));
}

IrType IrType::MakeSimpleType(Kind kind) {
  Node node;
  node.kind = kind;
  return IrType(std::make_shared<const Node>(std::move(node)));
}

IrType IrType::Scalar(ScalarKind kind) {
  return MakeScalarType(kind);
}

IrType IrType::Bool() {
  return MakeScalarType(ScalarKind::Bool);
}
IrType IrType::I32() {
  return MakeScalarType(ScalarKind::I32);
}
IrType IrType::U32() {
  return MakeScalarType(ScalarKind::U32);
}
IrType IrType::F32() {
  return MakeScalarType(ScalarKind::F32);
}

IrType IrType::Vec2(ScalarKind element) {
  return MakeVectorType(element, 2);
}
IrType IrType::Vec3(ScalarKind element) {
  return MakeVectorType(element, 3);
}
IrType IrType::Vec4(ScalarKind element) {
  return MakeVectorType(element, 4);
}

IrType IrType::Mat4x4f() {
  return MakeSimpleType(Kind::Matrix4x4f);
}

IrType IrType::Texture2dF32() {
  return MakeSimpleType(Kind::Texture2dF32);
}

IrType IrType::SamplerType() {
  return MakeSimpleType(Kind::Sampler);
}

ShaderResult<IrType> IrType::SizedArray(const IrType& element, uint32_t count,
                                        const RcString& label) {
  if (count == 0) {
    return ShaderError{"array element count must be nonzero", label};
  }
  if (!element.isPlainData()) {
    return ShaderError{
        std::format("array element type {} is not plain data (runtime arrays, textures, and "
                    "samplers cannot be array elements)",
                    element.toString()),
        label};
  }

  Node node;
  node.kind = Kind::SizedArray;
  node.element.push_back(element);
  node.arrayCount = count;
  return IrType(std::make_shared<const Node>(std::move(node)));
}

ShaderResult<IrType> IrType::RuntimeArray(const IrType& element, const RcString& label) {
  if (!element.isPlainData()) {
    return ShaderError{
        std::format("runtime array element type {} is not plain data", element.toString()), label};
  }

  Node node;
  node.kind = Kind::RuntimeArray;
  node.element.push_back(element);
  return IrType(std::make_shared<const Node>(std::move(node)));
}

ShaderResult<IrType> IrType::Struct(const RcString& name, std::vector<Member> members,
                                    const RcString& label) {
  if (members.empty()) {
    return ShaderError{std::format("struct {} must have at least one member", name.str()), label};
  }
  for (size_t i = 0; i < members.size(); ++i) {
    if (!members[i].type.isPlainData()) {
      return ShaderError{
          std::format("struct {} member {} has type {} which is not plain data (runtime arrays "
                      "are only supported as storage binding roots)",
                      name.str(), members[i].name.str(), members[i].type.toString()),
          label};
    }
    for (size_t j = i + 1; j < members.size(); ++j) {
      if (members[j].name == members[i].name) {
        return ShaderError{std::format("struct {} has duplicate member name {}", name.str(),
                                       members[i].name.str()),
                           label};
      }
    }
  }

  Node node;
  node.kind = Kind::Struct;
  node.structName = name;
  node.members = std::move(members);
  return IrType(std::make_shared<const Node>(std::move(node)));
}

IrType::Kind IrType::kind() const {
  return node_->kind;
}

ScalarKind IrType::scalarKind() const {
  return node_->scalar;
}

uint32_t IrType::vectorSize() const {
  return node_->vectorSize;
}

const IrType& IrType::elementType() const {
  return node_->element.front();
}

uint32_t IrType::arrayCount() const {
  return node_->arrayCount;
}

const RcString& IrType::structName() const {
  return node_->structName;
}

std::span<const IrType::Member> IrType::structMembers() const {
  return node_->members;
}

bool IrType::isNumericScalar() const {
  return kind() == Kind::Scalar && node_->scalar != ScalarKind::Bool;
}

bool IrType::isNumericVector() const {
  return kind() == Kind::Vector && node_->scalar != ScalarKind::Bool;
}

bool IrType::isFloatScalarOrVector() const {
  return (kind() == Kind::Scalar || kind() == Kind::Vector) && node_->scalar == ScalarKind::F32;
}

bool IrType::isPlainData() const {
  switch (kind()) {
    case Kind::Scalar:
    case Kind::Vector:
    case Kind::Matrix4x4f:
    case Kind::SizedArray:
    case Kind::Struct: return true;
    case Kind::RuntimeArray:
    case Kind::Texture2dF32:
    case Kind::Sampler: return false;
  }
  return false;
}

bool IrType::operator==(const IrType& other) const {
  if (node_ == other.node_) {
    return true;
  }
  if (node_->kind != other.node_->kind) {
    return false;
  }
  switch (node_->kind) {
    case Kind::Scalar: return node_->scalar == other.node_->scalar;
    case Kind::Vector:
      return node_->scalar == other.node_->scalar && node_->vectorSize == other.node_->vectorSize;
    case Kind::Matrix4x4f:
    case Kind::Texture2dF32:
    case Kind::Sampler: return true;
    case Kind::SizedArray:
      return node_->arrayCount == other.node_->arrayCount &&
             node_->element.front() == other.node_->element.front();
    case Kind::RuntimeArray: return node_->element.front() == other.node_->element.front();
    case Kind::Struct: {
      if (node_->structName != other.node_->structName ||
          node_->members.size() != other.node_->members.size()) {
        return false;
      }
      for (size_t i = 0; i < node_->members.size(); ++i) {
        if (node_->members[i].name != other.node_->members[i].name ||
            !(node_->members[i].type == other.node_->members[i].type)) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

std::string IrType::toString() const {
  switch (kind()) {
    case Kind::Scalar: {
      switch (node_->scalar) {
        case ScalarKind::Bool: return "bool";
        case ScalarKind::I32: return "i32";
        case ScalarKind::U32: return "u32";
        case ScalarKind::F32: return "f32";
      }
      return "unknown";
    }
    case Kind::Vector: {
      std::string element;
      switch (node_->scalar) {
        case ScalarKind::Bool: element = "bool"; break;
        case ScalarKind::I32: element = "i32"; break;
        case ScalarKind::U32: element = "u32"; break;
        case ScalarKind::F32: element = "f32"; break;
      }
      return std::format("vec{}<{}>", node_->vectorSize, element);
    }
    case Kind::Matrix4x4f: return "mat4x4<f32>";
    case Kind::SizedArray:
      return std::format("array<{}, {}>", node_->element.front().toString(), node_->arrayCount);
    case Kind::RuntimeArray: return std::format("array<{}>", node_->element.front().toString());
    case Kind::Struct: return node_->structName.str();
    case Kind::Texture2dF32: return "texture_2d<f32>";
    case Kind::Sampler: return "sampler";
  }
  return "unknown";
}

}  // namespace donner::gpu::shader
