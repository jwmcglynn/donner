#pragma once
/// @file
/// Bounded, typed intermediate representation for the supported WGSL frontend profile.

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

#include "donner/base/Utils.h"

namespace donner::gpu::shader::wgsl {

/// A half-open byte range in the WGSL input.
struct SourceSpan {
  uint32_t begin = 0;  //!< First byte in the range.
  uint32_t end = 0;    //!< First byte after the range.

  /// Returns true when this span has no bytes.
  constexpr bool empty() const { return begin == end; }
};

/// Identifier for an item stored in one of Module's arenas.
using ArenaId = uint16_t;

/// Invalid arena identifier.
inline constexpr ArenaId kInvalidArenaId = std::numeric_limits<ArenaId>::max();

/// A module-owned identifier spelling.
struct NameRef {
  uint16_t offset = 0;  //!< Offset in Module::identifierBytes.
  uint16_t length = 0;  //!< Byte count in Module::identifierBytes.
};

/// Storage format accepted by the frontend profile.
enum class StorageTextureFormat : uint8_t {
  Rgba32Float,  //!< `rgba32float`.
  Rgba8Unorm,   //!< `rgba8unorm`.
};

/// Kind of a resolved WGSL value type.
enum class TypeKind : uint8_t {
  Void,              //!< No value.
  Bool,              //!< `bool`.
  I32,               //!< `i32`.
  U32,               //!< `u32`.
  F32,               //!< `f32`.
  AbstractInt,       //!< Shader-creation-time signed integer.
  AbstractFloat,     //!< Shader-creation-time binary64 value.
  Struct,            //!< A declared structure.
  Matrix,            //!< A column-major f32 matrix.
  Array,             //!< A fixed-size array.
  Sampler,           //!< Filtering sampler resource.
  SampledTexture2d,  //!< `texture_2d<f32>`.
  StorageTexture2d,  //!< `texture_storage_2d<rgba32float, write>`.
};

/// A resolved scalar, vector, structure, or resource type.
struct Type {
  TypeKind kind = TypeKind::Void;      //!< Type category.
  uint8_t lanes = 1;                   //!< Vector lane count (1 through 4).
  ArenaId structId = kInvalidArenaId;  //!< Structure for TypeKind::Struct.
  StorageTextureFormat storageFormat = StorageTextureFormat::Rgba32Float;  //!< Storage format.
  TypeKind elementKind = TypeKind::Void;  //!< Array element category.
  uint8_t elementLanes = 1;               //!< Array element vector lane count.
  uint16_t arrayCount = 0;                //!< Fixed array element count.
  uint8_t columns = 1;                    //!< Matrix columns; one for non-matrices.
  uint8_t rows = 1;                       //!< Matrix rows; one for non-matrices.

  /// Returns the scalar/vector/structure element type of an array.
  constexpr Type elementType() const { return Type{elementKind, elementLanes, structId}; }

  /// Returns whether a storage texture's format belongs to this compiler profile.
  constexpr bool hasSupportedStorageFormat() const {
    return storageFormat == StorageTextureFormat::Rgba32Float ||
           storageFormat == StorageTextureFormat::Rgba8Unorm;
  }

  /// Returns true for identical resolved types.
  constexpr bool operator==(const Type& other) const = default;

  /// Returns whether scalar materialization is still required.
  constexpr bool isAbstract() const {
    return kind == TypeKind::AbstractInt || kind == TypeKind::AbstractFloat;
  }

  /// Returns true for scalar numeric types and vectors of them.
  constexpr bool isNumeric() const {
    return kind == TypeKind::I32 || kind == TypeKind::U32 || kind == TypeKind::F32;
  }
};

/// Builtin value supported by a shader entry interface.
enum class BuiltinValue : uint8_t {
  None,
  GlobalInvocationId,
  VertexIndex,
  Position,
};

/// A scalar/vector leaf in an entry-point interface.
struct InterfaceDecoration {
  BuiltinValue builtin = BuiltinValue::None;
  uint32_t location = UINT32_MAX;

  /// Returns whether this value is decorated as shader IO.
  constexpr bool present() const { return builtin != BuiltinValue::None || location != UINT32_MAX; }
};

/// Storage layout of one structure member.
struct StructMember {
  NameRef name;                   //!< Module-owned member name.
  SourceSpan nameSpan;            //!< Name location in the source.
  Type type;                      //!< Resolved member type.
  uint32_t offset = 0;            //!< Uniform-buffer byte offset.
  uint32_t alignment = 1;         //!< Uniform-buffer byte alignment.
  uint32_t size = 0;              //!< Uniform-buffer byte size.
  uint32_t arrayStride = 0;       //!< Storage-array byte stride, zero for non-arrays.
  InterfaceDecoration interface;  //!< Entry-point decoration, when this is an IO structure.
};

/// One declared structure and its computed uniform layout.
struct Struct {
  NameRef name;                           //!< Module-owned structure name.
  SourceSpan nameSpan;                    //!< Name location in the source.
  ArenaId firstMember = kInvalidArenaId;  //!< First StructMember arena item.
  uint16_t memberCount = 0;               //!< Number of members.
  uint32_t alignment = 1;                 //!< Uniform-buffer byte alignment.
  uint32_t size = 0;                      //!< Uniform-buffer byte size.
};

/// Module-scope resource binding kind.
enum class BindingKind : uint8_t {
  Uniform,          //!< `var<uniform>`.
  ReadOnlyStorage,  //!< `var<storage, read>`.
  Sampler,          //!< A filtering sampler.
  SampledTexture,   //!< `texture_2d<f32>`.
  StorageTexture,   //!< Write-only storage texture.
};

/// One module-scope resource binding.
struct Binding {
  BindingKind kind = BindingKind::Uniform;  //!< Resource category.
  Type type;                                //!< Resource type.
  NameRef name;                             //!< Module-owned binding name.
  SourceSpan nameSpan;                      //!< Name location in the source.
  uint32_t group = 0;                       //!< `@group` value.
  uint32_t binding = 0;                     //!< `@binding` value.
  ArenaId symbolId = kInvalidArenaId;       //!< Corresponding Symbol arena item.
};

/// Scope category of one resolved name.
enum class SymbolKind : uint8_t {
  Constant,   //!< A module-scope constant value.
  Binding,    //!< A module-scope resource binding.
  Parameter,  //!< An immutable function parameter.
  Let,        //!< An immutable local binding.
  Var,        //!< A mutable local variable.
};

/// One resolved identifier declaration.
struct Symbol {
  SymbolKind kind = SymbolKind::Let;             //!< Declaration category.
  Type type;                                     //!< Resolved declared type.
  NameRef name;                                  //!< Module-owned declaration name.
  SourceSpan nameSpan;                           //!< Name location in the source.
  bool mutableValue = false;                     //!< True only for a `var` declaration.
  ArenaId bindingId = kInvalidArenaId;           //!< Binding arena item for Binding symbols.
  BuiltinValue builtin = BuiltinValue::None;     //!< Entry-point parameter builtin.
  uint32_t location = UINT32_MAX;                //!< Entry-point parameter location, if present.
  ArenaId constantExpression = kInvalidArenaId;  //!< Initializer for module constants.
};

/// Unary operator represented by Expression::payload.
enum class UnaryOp : uint8_t {
  Negate,  //!< `-`.
  Not,     //!< `!`.
};

/// Binary operator represented by Expression::payload.
enum class BinaryOp : uint8_t {
  BitAnd = 13,  //!< Integer bitwise AND.
  Add = 0,
  Sub,  //!< `-`.
  Mul,  //!< `*`.
  Div,  //!< `/`.
  Mod,  //!< `%`.
  Lt,   //!< `<`.
  Le,   //!< `<=`.
  Gt,   //!< `>`.
  Ge,   //!< `>=`.
  Eq,   //!< `==`.
  Ne,   //!< `!=`.
  And,  //!< `&&`.
  Or,   //!< `||`.
};

/// Builtins accepted by the frontend profile.
enum class Builtin : uint8_t {
  All,
  Abs,
  Max,
  Round,
  Sqrt,
  Dot,
  Length,
  Normalize,
  Saturate,
  Fract,
  Fwidth,
  Any,                //!< `any`.
  Clamp,              //!< `clamp`.
  Select,             //!< `select`.
  Min,                //!< `min`.
  Ceil,               //!< `ceil`.
  Exp,                //!< `exp`.
  TextureLoad,        //!< `textureLoad`.
  TextureDimensions,  //!< `textureDimensions`.
  TextureStore,       //!< `textureStore`.
  Floor,              //!< Floating-point `floor`.
  Sign,               //!< Floating-point `sign`.
};

/// Kind of a typed expression node.
enum class ExpressionKind : uint8_t {
  Zero,          //!< Zero initialization of a constructible value.
  Literal,       //!< A scalar literal; payload holds IEEE or integer bits.
  Symbol,        //!< A resolved Symbol; payload is its arena identifier.
  Unary,         //!< A UnaryOp and one operand.
  Binary,        //!< A BinaryOp and two operands.
  Member,        //!< A structure member; payload is its StructMember identifier.
  Swizzle,       //!< Vector swizzle; payload encodes lane indices.
  Construct,     //!< Vector construction.
  Convert,       //!< Explicit scalar or vector conversion.
  BuiltinCall,   //!< A Builtin call.
  FunctionCall,  //!< A declared Function call; payload is its arena identifier.
  Index,         //!< Fixed-array access with base and index operands.
};

/// One typed expression node. Operands are in source order.
struct Expression {
  ExpressionKind kind = ExpressionKind::Literal;  //!< Node category.
  Type type;                                      //!< Statically resolved result type.
  SourceSpan span;                                //!< Source bytes covering this expression.
  std::array<ArenaId, 4> operands = {kInvalidArenaId, kInvalidArenaId, kInvalidArenaId,
                                     kInvalidArenaId};  //!< Child expressions.
  uint8_t operandCount = 0;                             //!< Number of valid operands.
  uint32_t payload = 0;                                 //!< Kind-specific bits or arena identifier.
  uint32_t literalHighBits = 0;  //!< Upper bits for abstract scalar literals.
};

/// Kind of a typed statement node.
enum class StatementKind : uint8_t {
  Declaration,   //!< A `let` or `var` declaration.
  Assign,        //!< An assignment.
  If,            //!< A conditional branch.
  For,           //!< A `for` loop.
  Return,        //!< A return statement.
  TextureStore,  //!< `textureStore(texture, coord, value)`.
  Break,         //!< Exit the innermost loop.
  Continue,      //!< Run the innermost loop continuation.
  Discard,       //!< Discard the current fragment invocation.
};

/// One typed statement node. The next link preserves lexical statement order.
struct Statement {
  StatementKind kind = StatementKind::Declaration;  //!< Node category.
  SourceSpan span;                                  //!< Source bytes covering this statement.
  ArenaId next = kInvalidArenaId;                   //!< Following statement in this block.
  ArenaId symbolId = kInvalidArenaId;               //!< Declared symbol for Declaration.
  ArenaId expression = kInvalidArenaId;             //!< Initializer, condition, return, or texture.
  ArenaId secondExpression = kInvalidArenaId;       //!< Assignment rhs, texture coordinate.
  ArenaId thirdExpression = kInvalidArenaId;        //!< Texture value.
  ArenaId firstBody = kInvalidArenaId;              //!< Then or loop body statement.
  ArenaId firstElseBody = kInvalidArenaId;          //!< Else body statement.
  ArenaId init = kInvalidArenaId;                   //!< For-loop initializer statement.
  ArenaId continuing = kInvalidArenaId;             //!< For-loop continuing statement.
};

/// Pipeline stage associated with a function.
enum class Stage : uint8_t {
  None,      //!< Helper function.
  Compute,   //!< Compute entry point.
  Vertex,    //!< Vertex entry point.
  Fragment,  //!< Fragment entry point.
};

/// A flattened scalar/vector input or output of one entry point.
struct InterfaceVariable {
  NameRef name;
  Type type;
  InterfaceDecoration decoration;
  ArenaId symbol = kInvalidArenaId;  //!< Parameter symbol, or invalid for a return value.
  ArenaId member = kInvalidArenaId;  //!< Structure member, or invalid for a direct value.
};

/// One declared function.
struct Function {
  NameRef name;                                       //!< Module-owned function name.
  SourceSpan nameSpan;                                //!< Name location in the source.
  Type returnType;                                    //!< Void for a function without a result.
  ArenaId firstParameter = kInvalidArenaId;           //!< First parameter Symbol arena item.
  uint16_t parameterCount = 0;                        //!< Number of parameters.
  ArenaId firstStatement = kInvalidArenaId;           //!< First body statement.
  Stage stage = Stage::None;                          //!< Helper or compute entry point.
  std::array<uint32_t, 3> workgroupSize = {1, 1, 1};  //!< Compute workgroup dimensions.
  InterfaceDecoration returnInterface;                //!< Direct return-value decoration.
  uint32_t resourceMask = 0;  //!< Resources statically accessed, including called helpers.
  uint16_t firstInput = 0;
  uint16_t inputCount = 0;
  uint16_t firstOutput = 0;
  uint16_t outputCount = 0;
};

/// Fixed capacities for one frontend module.
struct ModuleLimits {
  static constexpr uint32_t kMaxSourceBytes = 32768;
  static constexpr uint16_t kMaxTokens = 8192;
  static constexpr uint16_t kMaxIdentifierBytes = 8192;
  static constexpr uint16_t kMaxStructs = 8;
  static constexpr uint16_t kMaxStructMembers = 64;
  static constexpr uint16_t kMaxArrayElements = 8192;
  static constexpr uint16_t kMaxBindings = 16;
  static constexpr uint16_t kMaxSymbols = 512;
  static constexpr uint16_t kMaxExpressions = 2048;
  static constexpr uint16_t kMaxStatements = 512;
  static constexpr uint16_t kMaxFunctions = 32;
  static constexpr uint16_t kMaxNesting = 16;
  static constexpr uint16_t kMaxInterfaceVariables = 64;
};

/// A complete, immutable-on-success frontend module backed by fixed arenas.
struct Module {
  /// Returns true only after the parser completed every validation pass.
  constexpr bool isValid() const { return valid; }

  /// Returns a module-owned identifier spelling.
  constexpr std::string_view name(NameRef reference) const UTILS_LIFETIME_BOUND {
    return std::string_view(identifierBytes.data() + reference.offset, reference.length);
  }

  /// Returns the validated WGSL source retained for WGSL projection.
  constexpr std::string_view source() const UTILS_LIFETIME_BOUND {
    return std::string_view(sourceBytes.data(), sourceByteCount);
  }

  /// Returns natural host-shareable alignment, or zero for an unsupported type.
  constexpr uint32_t typeAlignment(Type type) const {
    if (type.isNumeric()) return type.lanes == 1 ? 4 : type.lanes == 2 ? 8 : 16;
    if (type.kind == TypeKind::Matrix) return type.rows == 2 ? 8 : 16;
    if (type.kind == TypeKind::Struct)
      return type.structId < structCount ? structs[type.structId].alignment : 0;
    if (type.kind == TypeKind::Array) return typeAlignment(type.elementType());
    return 0;
  }

  /// Returns fixed byte size; runtime arrays and unsupported types have size zero.
  constexpr uint32_t typeSize(Type type) const {
    if (type.isNumeric()) return 4u * type.lanes;
    if (type.kind == TypeKind::Matrix) return typeAlignment(type) * type.columns;
    if (type.kind == TypeKind::Struct)
      return type.structId < structCount ? structs[type.structId].size : 0;
    if (type.kind == TypeKind::Array) return arrayStride(type) * type.arrayCount;
    return 0;
  }

  /// Returns the array element stride, including trailing element padding.
  constexpr uint32_t arrayStride(Type type) const {
    const Type element = type.elementType();
    const uint32_t alignment = typeAlignment(element);
    const uint32_t size = typeSize(element);
    return alignment == 0 ? 0 : ((size + alignment - 1) / alignment) * alignment;
  }

  bool valid = false;  //!< Set only after complete successful parsing and validation.
  std::array<char, ModuleLimits::kMaxSourceBytes> sourceBytes = {};
  uint32_t sourceByteCount = 0;
  std::array<char, ModuleLimits::kMaxIdentifierBytes> identifierBytes = {};
  uint16_t identifierByteCount = 0;
  std::array<Struct, ModuleLimits::kMaxStructs> structs = {};
  uint16_t structCount = 0;
  std::array<StructMember, ModuleLimits::kMaxStructMembers> structMembers = {};
  uint16_t structMemberCount = 0;
  std::array<Binding, ModuleLimits::kMaxBindings> bindings = {};
  uint16_t bindingCount = 0;
  std::array<Symbol, ModuleLimits::kMaxSymbols> symbols = {};
  uint16_t symbolCount = 0;
  std::array<Expression, ModuleLimits::kMaxExpressions> expressions = {};
  uint16_t expressionCount = 0;
  std::array<Statement, ModuleLimits::kMaxStatements> statements = {};
  uint16_t statementCount = 0;
  std::array<Function, ModuleLimits::kMaxFunctions> functions = {};
  uint16_t functionCount = 0;
  std::array<InterfaceVariable, ModuleLimits::kMaxInterfaceVariables> interfaceVariables = {};
  uint16_t interfaceVariableCount = 0;
};

}  // namespace donner::gpu::shader::wgsl
