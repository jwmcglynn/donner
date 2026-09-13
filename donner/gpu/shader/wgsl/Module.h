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
};

/// Kind of a resolved WGSL value type.
enum class TypeKind : uint8_t {
  Void,              //!< No value.
  Bool,              //!< `bool`.
  I32,               //!< `i32`.
  U32,               //!< `u32`.
  F32,               //!< `f32`.
  Struct,            //!< A declared structure.
  Array,             //!< A fixed-size array.
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

  /// Returns true for identical resolved types.
  constexpr bool operator==(const Type& other) const = default;

  /// Returns true for scalar numeric types and vectors of them.
  constexpr bool isNumeric() const {
    return kind == TypeKind::I32 || kind == TypeKind::U32 || kind == TypeKind::F32;
  }
};

/// Storage layout of one structure member.
struct StructMember {
  NameRef name;              //!< Module-owned member name.
  SourceSpan nameSpan;       //!< Name location in the source.
  Type type;                 //!< Resolved member type.
  uint32_t offset = 0;       //!< Uniform-buffer byte offset.
  uint32_t alignment = 1;    //!< Uniform-buffer byte alignment.
  uint32_t size = 0;         //!< Uniform-buffer byte size.
  uint32_t arrayStride = 0;  //!< Storage-array byte stride, zero for non-arrays.
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
  Binding,    //!< A module-scope resource binding.
  Parameter,  //!< An immutable function parameter.
  Let,        //!< An immutable local binding.
  Var,        //!< A mutable local variable.
};

/// Builtin decoration supported on an entry-point parameter.
enum class BuiltinInput : uint8_t {
  None,                //!< An ordinary parameter.
  GlobalInvocationId,  //!< `@builtin(global_invocation_id)`.
};

/// One resolved identifier declaration.
struct Symbol {
  SymbolKind kind = SymbolKind::Let;          //!< Declaration category.
  Type type;                                  //!< Resolved declared type.
  NameRef name;                               //!< Module-owned declaration name.
  SourceSpan nameSpan;                        //!< Name location in the source.
  bool mutableValue = false;                  //!< True only for a `var` declaration.
  ArenaId bindingId = kInvalidArenaId;        //!< Binding arena item for Binding symbols.
  BuiltinInput builtin = BuiltinInput::None;  //!< Entry-point parameter decoration.
};

/// Unary operator represented by Expression::payload.
enum class UnaryOp : uint8_t {
  Negate,  //!< `-`.
  Not,     //!< `!`.
};

/// Binary operator represented by Expression::payload.
enum class BinaryOp : uint8_t {
  Add,  //!< `+`.
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
  Any,                //!< `any`.
  Clamp,              //!< `clamp`.
  Select,             //!< `select`.
  Min,                //!< `min`.
  Ceil,               //!< `ceil`.
  Exp,                //!< `exp`.
  TextureLoad,        //!< `textureLoad`.
  TextureDimensions,  //!< `textureDimensions`.
  TextureStore,       //!< `textureStore`.
};

/// Kind of a typed expression node.
enum class ExpressionKind : uint8_t {
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
};

/// Kind of a typed statement node.
enum class StatementKind : uint8_t {
  Declaration,   //!< A `let` or `var` declaration.
  Assign,        //!< An assignment.
  If,            //!< A conditional branch.
  For,           //!< A `for` loop.
  Return,        //!< A return statement.
  TextureStore,  //!< `textureStore(texture, coord, value)`.
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
  None,     //!< Helper function.
  Compute,  //!< Compute entry point.
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
};

/// Fixed capacities for one frontend module.
struct ModuleLimits {
  static constexpr uint32_t kMaxSourceBytes = 16384;
  static constexpr uint16_t kMaxTokens = 2048;
  static constexpr uint16_t kMaxIdentifierBytes = 2048;
  static constexpr uint16_t kMaxStructs = 8;
  static constexpr uint16_t kMaxStructMembers = 64;
  static constexpr uint16_t kMaxArrayElements = 256;
  static constexpr uint16_t kMaxBindings = 16;
  static constexpr uint16_t kMaxSymbols = 192;
  static constexpr uint16_t kMaxExpressions = 768;
  static constexpr uint16_t kMaxStatements = 256;
  static constexpr uint16_t kMaxFunctions = 16;
  static constexpr uint16_t kMaxNesting = 16;
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
};

}  // namespace donner::gpu::shader::wgsl
