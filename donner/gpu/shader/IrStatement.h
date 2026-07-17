#pragma once
/// @file
/// Statement nodes for the \c donner::gpu::shader IR.
///
/// Statements are immutable value nodes produced by \c FunctionBuilder, which performs all
/// scope, type, and stage validation before constructing them; the nodes themselves are plain
/// data consumed by serialization and (in later packets) the emitters.

#include <memory>
#include <optional>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/IrType.h"

namespace donner::gpu::shader {

class IrStmt;

/// An ordered list of statements.
using IrBlock = std::vector<IrStmt>;

/**
 * One IR statement. Construct through \c FunctionBuilder; this class is an immutable value
 * handle over validated statement data.
 */
class IrStmt {
public:
  /// Statement kind.
  enum class Kind : uint8_t {
    Let,       //!< `let name = expr;`
    Var,       //!< `var name: type (= init);`
    Assign,    //!< `lvalue = expr;`
    If,        //!< `if (cond) { ... } else { ... }`
    For,       //!< `for (init; cond; continuing) { ... }`
    Break,     //!< `break;`
    Continue,  //!< `continue;`
    Return,    //!< `return;` / `return expr;` / entry point output return.
    Discard,   //!< `discard;` (fragment stage only).
  };

  /// Internal storage; public for the builder and serialization implementation.
  struct Data {
    Kind kind = Kind::Break;                   //!< Statement kind.
    RcString name;                             //!< Let/var name.
    std::optional<IrType> declaredType;        //!< Var declared type.
    std::vector<IrExpr> exprs;                 //!< Kind-dependent expressions: let/var init, assign
                                               //!< (lhs, rhs), if/for condition, return values.
    IrBlock body;                              //!< If-then / for body.
    IrBlock elseBody;                          //!< If-else body.
    std::shared_ptr<const IrStmt> init;        //!< For-loop init statement.
    std::shared_ptr<const IrStmt> continuing;  //!< For-loop continuing statement.
  };

  /**
   * Wraps validated statement data; called by the builder only.
   *
   * @param data Statement payload.
   */
  explicit IrStmt(Data&& data) : data_(std::make_shared<const Data>(std::move(data))) {}

  /// Statement kind.
  Kind kind() const { return data_->kind; }

  /// Statement payload.
  const Data& data() const { return *data_; }

private:
  std::shared_ptr<const Data> data_;
};

}  // namespace donner::gpu::shader
