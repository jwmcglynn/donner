#pragma once
/// @file
/// Module model and validating builders for the \c donner::gpu::shader IR.
///
/// A module holds module-scope constants, resource bindings, and functions (plain functions
/// plus vertex/fragment entry points). \c ModuleBuilder and \c FunctionBuilder validate every
/// construction step and fail closed with \ref ShaderError on ill-typed input; a successfully
/// built \c IrModule is immutable and serializes deterministically.
///
/// Compute entry points are intentionally out of scope for this packet; \c StageKind leaves the
/// seam (a Compute enumerator would slot next to Vertex/Fragment, with workgroup-size metadata
/// on IrFunction) for the filter-engine migration packets.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/IrStatement.h"
#include "donner/gpu/shader/IrType.h"
#include "donner/gpu/shader/ShaderResult.h"

namespace donner::gpu::shader {

class ModuleBuilder;
class FunctionBuilder;

/// Kind of a module-scope resource binding.
enum class BindingKind : uint8_t {
  UniformBuffer,          //!< `var<uniform>` with a struct type.
  ReadOnlyStorageBuffer,  //!< `var<storage, read>` with a runtime array or struct type.
  SampledTexture2dF32,    //!< `texture_2d<f32>`.
  FilteringSampler,       //!< `sampler`.
};

/// Ostream output operator, e.g. `uniform`. @param os Output stream. @param value Value to
/// output.
std::ostream& operator<<(std::ostream& os, BindingKind value);

/// Pipeline stage of a function.
enum class StageKind : uint8_t {
  None,      //!< Plain (non-entry-point) function.
  Vertex,    //!< Vertex entry point.
  Fragment,  //!< Fragment entry point.
};

/// Builtin input values available to entry point parameters.
enum class BuiltinInput : uint8_t {
  InstanceIndex,  //!< `instance_index` (vertex stage, u32).
};

/// Builtin output values available to entry point outputs.
enum class BuiltinOutput : uint8_t {
  Position,  //!< Clip-space position (vertex stage, vec4<f32>).
};

/// One module-scope constant.
struct IrConstant {
  RcString name;  //!< Constant name.
  IrExpr value;   //!< Literal value (determines the type).
};

/// One module-scope resource binding.
struct IrBinding {
  uint32_t group = 0;                             //!< Bind group index.
  uint32_t binding = 0;                           //!< Binding index within the group.
  RcString name;                                  //!< Binding variable name.
  BindingKind kind = BindingKind::UniformBuffer;  //!< Binding kind.
  IrType type;  //!< Bound type (struct, runtime array, texture, or sampler).
};

/// One entry point parameter or plain function parameter.
struct IrParam {
  RcString name;                        //!< Parameter name.
  IrType type;                          //!< Parameter type.
  std::optional<uint32_t> location;     //!< Stage IO location (entry points).
  std::optional<BuiltinInput> builtin;  //!< Builtin input (entry points).
};

/// One entry point output member.
struct IrOutputMember {
  RcString name;                         //!< Output name.
  IrType type;                           //!< Output type.
  std::optional<uint32_t> location;      //!< Stage IO location.
  std::optional<BuiltinOutput> builtin;  //!< Builtin output.
};

/// One function: a plain function or an entry point.
struct IrFunction {
  RcString name;                        //!< Function name.
  StageKind stage = StageKind::None;    //!< Stage kind.
  std::vector<IrParam> params;          //!< Parameters, in order.
  std::optional<IrType> returnType;     //!< Plain function return type (empty = void).
  std::vector<IrOutputMember> outputs;  //!< Entry point outputs (empty for plain functions).
  IrBlock body;                         //!< Function body.
};

/**
 * An immutable, validated shader IR module.
 */
class IrModule {
public:
  /// Module-scope constants, in declaration order.
  const std::vector<IrConstant>& constants() const { return constants_; }
  /// Resource bindings, in declaration order.
  const std::vector<IrBinding>& bindings() const { return bindings_; }
  /// Functions (including entry points), in declaration order.
  const std::vector<IrFunction>& functions() const { return functions_; }

  /**
   * Serializes the module to a stable, line-based text dump for golden tests and debugging.
   * Byte-identical across runs and across semantically identical construction orderings:
   * iteration follows declaration order and the output contains no pointers or addresses.
   */
  std::string serialize() const;

private:
  friend class ModuleBuilder;

  IrModule() = default;

  std::vector<IrConstant> constants_;
  std::vector<IrBinding> bindings_;
  std::vector<IrFunction> functions_;
};

/**
 * Builds one function or entry point. Created by \c ModuleBuilder; validates scope, types,
 * loop/stage context, and stage IO before recording statements. The first error latches: every
 * subsequent operation and \ref finish return it, so an ill-typed build cannot silently succeed.
 */
class FunctionBuilder {
public:
  /// Move constructor; \p other becomes an inert moved-from shell.
  /// @param other Builder to move from.
  FunctionBuilder(FunctionBuilder&& other) noexcept;
  FunctionBuilder(const FunctionBuilder&) = delete;
  FunctionBuilder& operator=(const FunctionBuilder&) = delete;
  FunctionBuilder& operator=(FunctionBuilder&&) = delete;
  /// Destructor. Destroying an unfinished builder abandons the function, letting the module
  /// builder start a new one (the abandoned function is not registered).
  ~FunctionBuilder();

  /**
   * Resolves \p name to a reference expression: function parameters, lets, vars, then
   * module-scope constants and resource bindings.
   *
   * @param name Name to resolve.
   */
  ShaderResult<IrExpr> ref(const RcString& name) const;

  /**
   * Declares `let name = value;` and returns a reference to it.
   *
   * @param name Binding name; must be unique within the function.
   * @param value Initializer.
   */
  ShaderResult<IrExpr> addLet(const RcString& name, const IrExpr& value);

  /**
   * Declares `var name: type (= init);` and returns a (mutable) reference to it.
   *
   * @param name Variable name; must be unique within the function.
   * @param type Declared type; must be plain data.
   * @param init Optional initializer; must match \p type.
   */
  ShaderResult<IrExpr> addVar(const RcString& name, const IrType& type,
                              const std::optional<IrExpr>& init = std::nullopt);

  /**
   * Records `lhs = rhs;`. \p lhs must be a mutable lvalue (a `var` or a member/index/
   * single-component-swizzle chain rooted at one) and types must match.
   *
   * @param lhs Assignment target.
   * @param rhs Value.
   */
  ShaderStatus assign(const IrExpr& lhs, const IrExpr& rhs);

  /**
   * Opens `if (condition) { ... }`. Statements recorded until \ref elseBranch or \ref endIf go
   * to the then-branch.
   *
   * @param condition Bool condition.
   */
  ShaderStatus beginIf(const IrExpr& condition);
  /// Switches the innermost open `if` to its else-branch.
  ShaderStatus elseBranch();
  /// Closes the innermost open `if`.
  ShaderStatus endIf();

  /**
   * Opens `for (var name = init; ...)` and returns a reference to the loop variable. Follow
   * with \ref forCondition and \ref forContinuing, then body statements, then \ref endFor.
   *
   * @param name Loop variable name; must be unique within the function.
   * @param init Loop variable initializer.
   */
  ShaderResult<IrExpr> beginFor(const RcString& name, const IrExpr& init);
  /**
   * Sets the condition of the innermost open `for`.
   *
   * @param condition Bool condition.
   */
  ShaderStatus forCondition(const IrExpr& condition);
  /**
   * Sets the continuing assignment of the innermost open `for`.
   *
   * @param lhs Continuing assignment target (normally the loop variable).
   * @param rhs Continuing assignment value.
   */
  ShaderStatus forContinuing(const IrExpr& lhs, const IrExpr& rhs);
  /// Closes the innermost open `for`.
  ShaderStatus endFor();

  /// Records `break;` (valid inside a loop).
  ShaderStatus breakStmt();
  /// Records `continue;` (valid inside a loop).
  ShaderStatus continueStmt();

  /**
   * Records `return expr;` for a plain function with a return type.
   *
   * @param value Return value; must match the declared return type.
   */
  ShaderStatus returnValue(const IrExpr& value);
  /// Records `return;` for a void plain function.
  ShaderStatus returnVoid();
  /**
   * Records the entry point return: one expression per declared output member, in order.
   *
   * @param outputs Output values matching the entry point's output member types.
   */
  ShaderStatus returnOutputs(std::vector<IrExpr> outputs);
  /// Records `discard;` (fragment entry points only).
  ShaderStatus discard();

  /**
   * Calls a previously finished module function.
   *
   * @param name Callee name.
   * @param args Arguments; must match the callee's parameter types.
   */
  ShaderResult<IrExpr> callFunction(const RcString& name, std::vector<IrExpr> args);

  /// Finishes the function, registering it with the module builder. Fails if any prior
  /// operation failed or a block is still open.
  ShaderStatus finish();

private:
  friend class ModuleBuilder;

  /// Frame of the open-block stack.
  struct BlockFrame {
    enum class Kind : uint8_t { IfThen, IfElse, ForBody } kind = Kind::IfThen;  //!< Frame kind.
    std::optional<IrExpr> condition;                                            //!< If condition.
    IrBlock statements;                  //!< Statements recorded into this frame.
    IrBlock thenStatements;              //!< Completed then-branch (IfElse frames).
    std::optional<IrStmt> init;          //!< For-loop init.
    std::optional<IrExpr> forCondition;  //!< For-loop condition (set via forCondition()).
    std::optional<IrStmt> continuing;    //!< For-loop continuing.
    bool forHeaderComplete = false;      //!< True once forContinuing() was called.
  };

  FunctionBuilder(ModuleBuilder& moduleBuilder, IrFunction&& function);

  /// Latches the first error and returns it.
  ShaderStatus fail(ShaderError&& error);
  /// Returns the latched error or nullopt.
  std::optional<ShaderError> checkUsable() const;
  /// Appends to the innermost open block (or the function body).
  void append(IrStmt&& statement);
  /// Declares a local name, rejecting duplicates.
  ShaderStatus declareName(const RcString& name, const IrType& type, RefKind kind);
  /// True if any enclosing open block is a loop body.
  bool insideLoop() const;

  ModuleBuilder* moduleBuilder_;
  IrFunction function_;
  std::optional<ShaderError> firstError_;
  bool finished_ = false;
  std::vector<BlockFrame> blockStack_;
  std::map<RcString, std::pair<IrType, RefKind>> locals_;
};

/**
 * Builds an \ref IrModule: module-scope constants, resource bindings with (group, binding)
 * uniqueness validation, and functions.
 */
class ModuleBuilder {
public:
  /// Constructs an empty module builder.
  ModuleBuilder() = default;

  ModuleBuilder(const ModuleBuilder&) = delete;
  ModuleBuilder& operator=(const ModuleBuilder&) = delete;
  ModuleBuilder(ModuleBuilder&&) = delete;
  ModuleBuilder& operator=(ModuleBuilder&&) = delete;
  /// Destructor.
  ~ModuleBuilder() = default;

  /**
   * Adds a module-scope constant with a literal value (e.g. a u32 sentinel).
   *
   * @param name Constant name; must be unique at module scope.
   * @param value Literal expression.
   */
  ShaderStatus addConstant(const RcString& name, const IrExpr& value);

  /**
   * Adds a uniform buffer binding.
   *
   * @param group Bind group index.
   * @param binding Binding index; (group, binding) must be unique.
   * @param name Binding name; must be unique at module scope.
   * @param structType Uniform struct type.
   */
  ShaderStatus addUniformBuffer(uint32_t group, uint32_t binding, const RcString& name,
                                const IrType& structType);

  /**
   * Adds a read-only storage buffer binding with a runtime array or struct type.
   *
   * @param group Bind group index.
   * @param binding Binding index; (group, binding) must be unique.
   * @param name Binding name; must be unique at module scope.
   * @param type Runtime array or struct type.
   */
  ShaderStatus addReadOnlyStorageBuffer(uint32_t group, uint32_t binding, const RcString& name,
                                        const IrType& type);

  /**
   * Adds a `texture_2d<f32>` binding.
   *
   * @param group Bind group index.
   * @param binding Binding index; (group, binding) must be unique.
   * @param name Binding name; must be unique at module scope.
   */
  ShaderStatus addTexture2d(uint32_t group, uint32_t binding, const RcString& name);

  /**
   * Adds a filtering sampler binding.
   *
   * @param group Bind group index.
   * @param binding Binding index; (group, binding) must be unique.
   * @param name Binding name; must be unique at module scope.
   */
  ShaderStatus addSampler(uint32_t group, uint32_t binding, const RcString& name);

  /**
   * Starts a plain function. Finish it with `FunctionBuilder::finish()` before starting the
   * next function.
   *
   * @param name Function name; must be unique at module scope.
   * @param params Parameters (types must be plain data or texture/sampler for helpers).
   * @param returnType Return type, or empty for void.
   */
  ShaderResult<FunctionBuilder> createFunction(const RcString& name, std::vector<IrParam> params,
                                               const std::optional<IrType>& returnType);

  /**
   * Starts a vertex entry point. Parameters may carry the `instance_index` builtin (u32) or
   * unique locations; outputs must contain exactly one `position` builtin (vec4<f32>) plus
   * uniquely-located outputs.
   *
   * @param name Entry point name; must be unique at module scope.
   * @param params Stage inputs.
   * @param outputs Stage outputs.
   */
  ShaderResult<FunctionBuilder> createVertexEntryPoint(const RcString& name,
                                                       std::vector<IrParam> params,
                                                       std::vector<IrOutputMember> outputs);

  /**
   * Starts a fragment entry point. Parameters must have unique locations; outputs must include
   * a location-0 `vec4<f32>` color.
   *
   * @param name Entry point name; must be unique at module scope.
   * @param params Stage inputs.
   * @param outputs Stage outputs.
   */
  ShaderResult<FunctionBuilder> createFragmentEntryPoint(const RcString& name,
                                                         std::vector<IrParam> params,
                                                         std::vector<IrOutputMember> outputs);

  /// Finalizes and returns the module. Fails if any recorded error is pending.
  ShaderResult<IrModule> build();

private:
  friend class FunctionBuilder;

  /// Resolves a module-scope name (constant or binding) to a reference expression.
  std::optional<IrExpr> resolveModuleName(const RcString& name) const;
  /// Looks up a finished function by name.
  const IrFunction* findFunction(const RcString& name) const;
  /// Validates module-scope name uniqueness.
  ShaderStatus checkModuleName(const RcString& name);
  /// Validates (group, binding) uniqueness and registers the binding.
  ShaderStatus addBinding(IrBinding&& binding);
  /// Registers a finished function (called by FunctionBuilder::finish).
  void registerFunction(IrFunction&& function);

  IrModule module_;
  bool functionInProgress_ = false;
};

}  // namespace donner::gpu::shader
