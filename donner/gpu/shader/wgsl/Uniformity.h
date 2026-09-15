#pragma once
/// @file
/// Bounded value and control dependencies for shader collective operations.

#include <array>
#include <cstdint>

#include "donner/gpu/shader/wgsl/Module.h"

namespace donner::gpu::shader::wgsl {

/// Uniformity validation failures, separate from syntax and type errors.
enum class UniformityError : uint8_t {
  None,
  InvalidModule,
  Limit,
  NonUniformControl,
  InvalidStage,
  AfterDiscard,
};

/// Result of the bounded dependency analysis.
struct UniformityResult {
  UniformityError error = UniformityError::None;
  SourceSpan span;
};

namespace uniformity_detail {
using Mask = uint32_t;
using NodeId = uint16_t;
inline constexpr NodeId kNone = UINT16_MAX;
inline constexpr Mask kControl = 1u << Expression::kMaxOperands;
inline constexpr Mask kNonUniform = kControl << 1;
inline constexpr uint8_t kNext = 1, kReturn = 2, kBreak = 4, kContinue = 8;
inline constexpr uint8_t kAllStages = 7, kCompute = 1, kVertex = 2, kFragment = 4;

struct Node {
  Mask seed = 0;
  Mask value = 0;
  NodeId firstEdge = kNone;
};
struct Edge {
  NodeId dependency = kNone;
  NodeId next = kNone;
};

/// Forward edges represent loop-carried values; masks grow monotonically to a fixed point.
class Graph {
public:
  constexpr void reset() {
    nodeCount_ = edgeCount_ = 0;
    failed_ = false;
    make();
  }
  constexpr NodeId make(Mask seed = 0, NodeId first = kNone, NodeId second = kNone) {
    if (nodeCount_ == nodes_.size()) {
      failed_ = true;
      return kNone;
    }
    const NodeId result = nodeCount_++;
    nodes_[result] = {seed, seed, kNone};
    depend(result, first);
    depend(result, second);
    return result;
  }
  constexpr void depend(NodeId node, NodeId dependency) {
    if (dependency == kNone) return;
    if (node >= nodeCount_ || dependency >= nodeCount_ || edgeCount_ == edges_.size()) {
      failed_ = true;
      return;
    }
    edges_[edgeCount_] = {dependency, nodes_[node].firstEdge};
    nodes_[node].firstEdge = edgeCount_++;
  }
  constexpr NodeId join(NodeId first, NodeId second) {
    if (first == kNone || first == second) return second;
    if (second == kNone) return first;
    if (first == 0) return second;
    if (second == 0) return first;
    return make(0, first, second);
  }
  constexpr Mask value(NodeId node) const { return node < nodeCount_ ? nodes_[node].value : 0; }
  constexpr bool solve(uint32_t& work, uint32_t limit) {
    bool changed = true;
    while (changed && !failed_) {
      changed = false;
      for (NodeId i = 0; i < nodeCount_; ++i) {
        Mask next = nodes_[i].seed;
        for (NodeId edge = nodes_[i].firstEdge; edge != kNone; edge = edges_[edge].next) {
          if (++work > limit) return false;
          next |= nodes_[edges_[edge].dependency].value;
        }
        if (next != nodes_[i].value) {
          nodes_[i].value = next;
          changed = true;
        }
      }
    }
    return !failed_;
  }

private:
  static constexpr uint16_t kNodes = 8192;
  static constexpr uint16_t kEdges = 32768;
  std::array<Node, kNodes> nodes_{};
  std::array<Edge, kEdges> edges_{};
  NodeId nodeCount_ = 0;
  NodeId edgeCount_ = 0;
  bool failed_ = false;
};

struct State {
  std::array<NodeId, ModuleLimits::kMaxSymbols> values{};
  NodeId control = 0;
  bool possibleDiscard = false;
};
struct Join {
  State state;
  bool reached = false;
};
struct Context {
  Join* breaks = nullptr;
  Join* continues = nullptr;
};
struct Requirement {
  NodeId dependencies = kNone;
  SourceSpan span;
};
struct Summary {
  Mask result = 0;
  Mask required = 0;
  /// What each pointer parameter's pointee depends on when the function returns.
  std::array<Mask, Expression::kMaxOperands> pointerEscape{};
  uint8_t stages = kAllStages;
  bool collective = false;
  bool discard = false;
};

class Analyzer {
public:
  constexpr explicit Analyzer(const Module& module, uint32_t workLimit)
      : module_(module), workLimit_(workLimit) {}

  constexpr UniformityResult run() {
    for (function_ = 0; function_ < module_.functionCount && ok(); ++function_) analyzeFunction();
    return result_;
  }

private:
  constexpr bool ok() const { return result_.error == UniformityError::None; }
  constexpr void fail(UniformityError error, SourceSpan span) {
    if (ok()) result_ = {error, span};
  }
  constexpr bool step(SourceSpan span) {
    if (++work_ > workLimit_) fail(UniformityError::Limit, span);
    return ok();
  }
  constexpr void copy(State& destination, const State& source) const {
    for (ArenaId i = firstSymbol_; i < endSymbol_; ++i) destination.values[i] = source.values[i];
    destination.control = source.control;
    destination.possibleDiscard = source.possibleDiscard;
  }
  constexpr void merge(Join& join, const State& incoming) {
    if (!join.reached) {
      copy(join.state, incoming);
      join.reached = true;
      return;
    }
    for (ArenaId i = firstSymbol_; i < endSymbol_; ++i)
      join.state.values[i] = graph_.join(join.state.values[i], incoming.values[i]);
    join.state.control = graph_.join(join.state.control, incoming.control);
    join.state.possibleDiscard |= incoming.possibleDiscard;
  }
  constexpr NodeId substitute(Mask mask, const Expression& call,
                              const std::array<NodeId, Expression::kMaxOperands>& arguments) {
    if (mask == 0) return 0;
    const NodeId result = graph_.make(mask & kNonUniform);
    if (mask & kControl) graph_.depend(result, state_.control);
    for (uint8_t i = 0; i < call.operandCount; ++i)
      if (mask & (1u << i)) graph_.depend(result, arguments[i]);
    return result;
  }
  constexpr void require(NodeId dependencies, SourceSpan span) {
    summary_.collective = true;
    if (state_.possibleDiscard) fail(UniformityError::AfterDiscard, span);
    if (requirementCount_ == requirements_.size()) {
      fail(UniformityError::Limit, span);
      return;
    }
    requirements_[requirementCount_++] = {dependencies, span};
  }

  constexpr NodeId expression(ArenaId id, uint16_t depth = 0) {
    if (id == kInvalidArenaId) return 0;
    if (id >= module_.expressionCount || depth >= 128) {
      fail(UniformityError::InvalidModule, {});
      return 0;
    }
    const Expression& node = module_.expressions[id];
    if (!step(node.span)) return 0;
    if (node.kind == ExpressionKind::Zero || node.kind == ExpressionKind::Literal) return 0;
    if (node.kind == ExpressionKind::Symbol) return symbol(node);
    if (node.operandCount > node.operands.size()) {
      fail(UniformityError::InvalidModule, node.span);
      return 0;
    }
    return operands(node, depth);
  }

  constexpr bool shortCircuit(const Expression& node) const {
    return node.kind == ExpressionKind::Binary &&
           (node.payload == uint32_t(BinaryOp::And) || node.payload == uint32_t(BinaryOp::Or));
  }

  constexpr NodeId operands(const Expression& node, uint16_t depth) {
    std::array<NodeId, Expression::kMaxOperands> arguments{};
    NodeId value = 0;
    const NodeId control = state_.control;
    for (uint8_t i = 0; i < node.operandCount; ++i) {
      if (i == 1 && shortCircuit(node)) state_.control = graph_.join(control, arguments[0]);
      arguments[i] = expression(node.operands[i], depth + 1);
      value = graph_.join(value, arguments[i]);
    }
    state_.control = control;
    if (node.kind == ExpressionKind::FunctionCall) return call(node, arguments);
    return node.kind == ExpressionKind::BuiltinCall ? builtin(node, value) : value;
  }

  constexpr NodeId builtin(const Expression& node, NodeId value) {
    const auto builtin = static_cast<Builtin>(node.payload);
    if (builtin == Builtin::Fwidth || builtin == Builtin::TextureSample) {
      summary_.stages &= kFragment;
      require(state_.control, node.span);
    }
    if (builtin == Builtin::TextureLoad || builtin == Builtin::TextureSample ||
        builtin == Builtin::TextureSampleLevel)
      value = graph_.make(kNonUniform, value);
    return value;
  }

  constexpr NodeId symbol(const Expression& node) {
    if (node.payload >= module_.symbolCount) {
      fail(UniformityError::InvalidModule, node.span);
      return 0;
    }
    const Symbol& symbol = module_.symbols[node.payload];
    if (symbol.kind == SymbolKind::Binding) {
      if (symbol.bindingId >= module_.bindingCount) {
        fail(UniformityError::InvalidModule, node.span);
        return 0;
      }
      return module_.bindings[symbol.bindingId].kind == BindingKind::ReadOnlyStorage
                 ? graph_.make(kNonUniform)
                 : 0;
    }
    if (node.payload < firstSymbol_ || node.payload >= endSymbol_ ||
        state_.values[node.payload] == kNone) {
      fail(UniformityError::InvalidModule, node.span);
      return 0;
    }
    return state_.values[node.payload];
  }

  constexpr NodeId call(const Expression& node,
                        const std::array<NodeId, Expression::kMaxOperands>& arguments) {
    if (node.payload >= function_) {
      fail(UniformityError::InvalidModule, node.span);
      return 0;
    }
    const Summary& callee = summaries_[node.payload];
    summary_.stages &= callee.stages;
    if (callee.collective) require(substitute(callee.required, node, arguments), node.span);
    state_.possibleDiscard |= callee.discard;
    summary_.discard |= callee.discard;
    escapePointers(callee, node, arguments);
    return substitute(callee.result, node, arguments);
  }

  /// Taints every variable whose address reaches a call with all of that call's dependencies.
  ///
  /// Writes through a pointer parameter are invisible in the callee summary, so the whole pointee
  /// conservatively depends on the arguments and the control that reached the call.
  constexpr bool pointerOperand(ArenaId id) const {
    return id < module_.expressionCount && module_.expressions[id].type.kind == TypeKind::Pointer;
  }

  /// Applies each pointer parameter's recorded escape mask to the variable the caller passed.
  ///
  /// The mask carries what the callee left in the pointee, including non-uniform sources it read
  /// itself, so a write through a pointer taints the caller exactly as the inlined write would.
  /// The scan runs before any graph edge so a call without pointer arguments allocates nothing.
  constexpr void escapePointers(const Summary& callee, const Expression& node,
                                const std::array<NodeId, Expression::kMaxOperands>& arguments) {
    bool escapes = false;
    for (uint8_t i = 0; i < node.operandCount && !escapes; ++i)
      escapes = pointerOperand(node.operands[i]);
    if (!escapes) return;
    for (uint8_t i = 0; i < node.operandCount; ++i) {
      if (!pointerOperand(node.operands[i])) continue;
      NodeId indices = 0;
      const ArenaId target = rootSymbol(node.operands[i], indices);
      if (target < firstSymbol_ || target >= endSymbol_) {
        fail(UniformityError::InvalidModule, node.span);
        return;
      }
      const NodeId escaped = substitute(callee.pointerEscape[i], node, arguments);
      state_.values[target] = graph_.join(
          state_.values[target], graph_.join(escaped, graph_.join(indices, state_.control)));
    }
  }

  constexpr ArenaId rootSymbol(ArenaId id, NodeId& indexDependencies) {
    for (uint16_t depth = 0; depth < 128 && id < module_.expressionCount; ++depth) {
      const Expression& node = module_.expressions[id];
      if (node.kind == ExpressionKind::Symbol) return static_cast<ArenaId>(node.payload);
      if (node.kind == ExpressionKind::Index)
        indexDependencies = graph_.join(indexDependencies, expression(node.operands[1]));
      if (node.operandCount == 0) break;
      id = node.operands[0];
    }
    fail(UniformityError::InvalidModule, {});
    return kInvalidArenaId;
  }

  constexpr uint8_t block(ArenaId first, Context context) {
    if (++blockDepth_ > ModuleLimits::kMaxNesting) {
      --blockDepth_;
      fail(UniformityError::Limit, {});
      return 0;
    }
    uint8_t behaviors = kNext;
    uint16_t count = 0;
    for (ArenaId current = first; current != kInvalidArenaId && (behaviors & kNext) && ok();
         current = module_.statements[current].next) {
      if (current >= module_.statementCount || ++count > ModuleLimits::kMaxStatements) {
        fail(UniformityError::InvalidModule, {});
        break;
      }
      behaviors = (behaviors & ~kNext) | statement(module_.statements[current], context);
    }
    --blockDepth_;
    return behaviors;
  }

  constexpr uint8_t statement(const Statement& node, Context context) {
    if (!step(node.span)) return 0;
    switch (node.kind) {
      case StatementKind::Declaration: return declaration(node);
      case StatementKind::Assign: return assignment(node);
      case StatementKind::If: return selection(node, context);
      case StatementKind::Switch: return switchStatement(node, context);
      case StatementKind::For:
      case StatementKind::While:
      case StatementKind::Loop: return loop(node, context);
      case StatementKind::Call: expression(node.expression); return kNext;
      case StatementKind::Return:
        returned_ =
            graph_.join(returned_, graph_.join(expression(node.expression), state_.control));
        recordPointerExits();
        return kReturn;
      default: return controlStatement(node, context);
    }
  }

  constexpr uint8_t declaration(const Statement& node) {
    if (node.symbolId < firstSymbol_ || node.symbolId >= endSymbol_) {
      fail(UniformityError::InvalidModule, node.span);
      return 0;
    }
    state_.values[node.symbolId] = graph_.join(expression(node.expression), state_.control);
    return kNext;
  }

  constexpr uint8_t assignment(const Statement& node) {
    NodeId indices = 0;
    const ArenaId target = rootSymbol(node.expression, indices);
    if (target < firstSymbol_ || target >= endSymbol_) {
      fail(UniformityError::InvalidModule, node.span);
      return 0;
    }
    NodeId value =
        graph_.join(expression(node.secondExpression), graph_.join(indices, state_.control));
    if (module_.expressions[node.expression].kind != ExpressionKind::Symbol)
      value = graph_.join(value, state_.values[target]);
    state_.values[target] = value;
    return kNext;
  }

  constexpr uint8_t controlStatement(const Statement& node, Context context) {
    switch (node.kind) {
      case StatementKind::Break:
        if (!context.breaks)
          fail(UniformityError::InvalidModule, node.span);
        else
          merge(*context.breaks, state_);
        return kBreak;
      case StatementKind::Continue:
        if (!context.continues)
          fail(UniformityError::InvalidModule, node.span);
        else
          merge(*context.continues, state_);
        return kContinue;
      case StatementKind::Discard:
        summary_.stages &= kFragment;
        summary_.discard = state_.possibleDiscard = true;
        return kNext;
      case StatementKind::TextureStore:
        summary_.stages &= kCompute;
        expression(node.expression);
        expression(node.secondExpression);
        expression(node.thirdExpression);
        return kNext;
      default: fail(UniformityError::InvalidModule, node.span); return 0;
    }
    return 0;
  }

  constexpr uint8_t selection(const Statement& node, Context context) {
    const NodeId condition = expression(node.expression);
    State incoming;
    copy(incoming, state_);
    state_.control = graph_.join(incoming.control, condition);
    const uint8_t yes = block(node.firstBody, context);
    Join normal;
    if (yes & kNext) merge(normal, state_);
    copy(state_, incoming);
    state_.control = graph_.join(incoming.control, condition);
    const uint8_t no = block(node.firstElseBody, context);
    if (no & kNext) merge(normal, state_);
    if (normal.reached) {
      copy(state_, normal.state);
      if (yes == kNext && no == kNext) state_.control = incoming.control;
    }
    return yes | no;
  }

  constexpr uint8_t switchStatement(const Statement& node, Context context) {
    const NodeId selector = expression(node.expression);
    State incoming;
    copy(incoming, state_);
    Join normal;
    uint8_t behaviors = 0;
    bool hasDefault = false;
    uint16_t count = 0;
    for (ArenaId id = node.firstBody; id != kInvalidArenaId && ok();
         id = module_.statements[id].next) {
      if (id >= module_.statementCount || ++count > ModuleLimits::kMaxStatements ||
          module_.statements[id].kind != StatementKind::Case) {
        fail(UniformityError::InvalidModule, node.span);
        break;
      }
      const Statement& clause = module_.statements[id];
      hasDefault |= clause.expression == kInvalidArenaId;
      copy(state_, incoming);
      state_.control = graph_.join(incoming.control, selector);
      const uint8_t branch = block(clause.firstBody, Context{&normal, context.continues});
      behaviors |= branch;
      if (branch & kNext) merge(normal, state_);
    }
    if (!hasDefault) {
      copy(state_, incoming);
      state_.control = graph_.join(incoming.control, selector);
      merge(normal, state_);
    }
    if (normal.reached) {
      copy(state_, normal.state);
      if (!(behaviors & (kReturn | kContinue))) state_.control = incoming.control;
    }
    return (behaviors & (kReturn | kContinue)) | (normal.reached ? kNext : 0);
  }

  /// Returns whether a declaration can change across loop iterations.
  ///
  /// A pointer parameter is written through `*p`, so its pointee is loop-carried even though the
  /// pointer itself is immutable.
  static constexpr bool loopCarried(const Symbol& symbol) {
    return symbol.kind == SymbolKind::Var ||
           (symbol.kind == SymbolKind::Parameter && symbol.type.kind == TypeKind::Pointer);
  }

  constexpr std::array<NodeId, ModuleLimits::kMaxSymbols> loopValues() {
    std::array<NodeId, ModuleLimits::kMaxSymbols> phis{};
    for (ArenaId i = firstSymbol_; i < endSymbol_; ++i) {
      phis[i] = kNone;
      if (loopCarried(module_.symbols[i]) && state_.values[i] != kNone) {
        phis[i] = graph_.make(0, state_.values[i]);
        state_.values[i] = phis[i];
      }
    }
    return phis;
  }

  constexpr void loopBackedge(NodeId header,
                              const std::array<NodeId, ModuleLimits::kMaxSymbols>& phis,
                              uint16_t firstRequirement, Join& exits) {
    graph_.depend(header, state_.control);
    for (ArenaId i = firstSymbol_; i < endSymbol_; ++i)
      if (phis[i] != kNone) graph_.depend(phis[i], state_.values[i]);
    if (state_.possibleDiscard && requirementCount_ > firstRequirement)
      fail(UniformityError::AfterDiscard, requirements_[firstRequirement].span);
    exits.state.possibleDiscard |= state_.possibleDiscard;
  }

  constexpr uint8_t loop(const Statement& node, Context context) {
    const bool counted = node.kind == StatementKind::For;
    // Only `loop` lacks a condition, so it is the one form that cannot skip its body.
    const bool conditional = node.kind != StatementKind::Loop;
    if (counted &&
        (node.init >= module_.statementCount || node.continuing >= module_.statementCount)) {
      fail(UniformityError::InvalidModule, node.span);
      return 0;
    }
    if (counted) statement(module_.statements[node.init], context);
    State incoming;
    copy(incoming, state_);
    const auto phis = loopValues();
    const NodeId header = graph_.make(0, incoming.control);
    state_.control = header;
    const uint16_t firstRequirement = requirementCount_;
    const NodeId condition = conditional ? expression(node.expression) : 0;
    state_.control = graph_.join(header, condition);
    const NodeId bodyControl = state_.control;
    Join exits, continuing;
    if (conditional) merge(exits, state_);
    const uint8_t body = block(node.firstBody, Context{&exits, &continuing});
    if (body & kNext) merge(continuing, state_);
    if (continuing.reached) {
      copy(state_, continuing.state);
      if (!(body & (kReturn | kBreak))) state_.control = bodyControl;
      if (counted) statement(module_.statements[node.continuing], context);
      loopBackedge(header, phis, firstRequirement, exits);
    }
    if (!exits.reached) return body & kReturn;
    copy(state_, exits.state);
    if (!(body & kReturn)) state_.control = incoming.control;
    return kNext | (body & kReturn);
  }

  constexpr void analyzeFunction() {
    const Function& function = module_.functions[function_];
    firstSymbol_ = function.firstParameter;
    endSymbol_ = function_ + 1 < module_.functionCount
                     ? module_.functions[function_ + 1].firstParameter
                     : module_.symbolCount;
    if (firstSymbol_ > endSymbol_ || endSymbol_ > module_.symbolCount) {
      fail(UniformityError::InvalidModule, function.nameSpan);
      return;
    }
    initializeFunction(function);
    block(function.firstStatement, {});
    recordPointerExits();
    if (!ok()) return;
    if (!graph_.solve(work_, workLimit_)) {
      fail(UniformityError::Limit, function.nameSpan);
      return;
    }
    finishFunction(function);
  }

  /// Joins each pointer parameter's current pointee into the value seen by the caller on exit.
  constexpr void recordPointerExits() {
    for (uint16_t i = 0; i < pointerParameters_; ++i) {
      if (!pointerParameter_[i]) continue;
      pointerExit_[i] = graph_.join(pointerExit_[i], state_.values[firstSymbol_ + i]);
    }
  }

  constexpr void initializeFunction(const Function& function) {
    graph_.reset();
    returned_ = kNone;
    summary_ = {};
    requirementCount_ = 0;
    state_.possibleDiscard = false;
    state_.control = graph_.make(function.stage == Stage::None ? kControl : 0);
    for (ArenaId i = firstSymbol_; i < endSymbol_; ++i) state_.values[i] = kNone;
    pointerParameters_ = function.parameterCount < Expression::kMaxOperands
                             ? function.parameterCount
                             : Expression::kMaxOperands;
    for (uint16_t i = 0; i < function.parameterCount; ++i) {
      state_.values[firstSymbol_ + i] =
          graph_.make(function.stage == Stage::None ? 1u << i : kNonUniform);
      if (i < pointerParameters_) {
        pointerParameter_[i] = module_.symbols[firstSymbol_ + i].type.kind == TypeKind::Pointer;
        pointerExit_[i] = kNone;
      }
    }
  }

  constexpr void finishFunction(const Function& function) {
    summary_.result = graph_.value(returned_);
    for (uint16_t i = 0; i < pointerParameters_; ++i)
      if (pointerParameter_[i]) summary_.pointerEscape[i] = graph_.value(pointerExit_[i]);
    for (uint16_t i = 0; i < requirementCount_; ++i) {
      const Mask mask = graph_.value(requirements_[i].dependencies);
      if (mask & kNonUniform) fail(UniformityError::NonUniformControl, requirements_[i].span);
      summary_.required |= mask;
    }
    const uint8_t stage = function.stage == Stage::Compute  ? kCompute
                          : function.stage == Stage::Vertex ? kVertex
                                                            : kFragment;
    if (function.stage != Stage::None && !(summary_.stages & stage))
      fail(UniformityError::InvalidStage, function.nameSpan);
    summaries_[function_] = summary_;
  }

  const Module& module_;
  const uint32_t workLimit_;
  Graph graph_;
  State state_;
  Summary summary_;
  std::array<Summary, ModuleLimits::kMaxFunctions> summaries_{};
  std::array<Requirement, ModuleLimits::kMaxExpressions> requirements_{};
  uint16_t requirementCount_ = 0;
  ArenaId function_ = 0;
  ArenaId firstSymbol_ = 0, endSymbol_ = 0;
  std::array<bool, Expression::kMaxOperands> pointerParameter_{};
  std::array<NodeId, Expression::kMaxOperands> pointerExit_{};
  uint16_t pointerParameters_ = 0;
  NodeId returned_ = kNone;
  uint32_t work_ = 0;
  uint16_t blockDepth_ = 0;
  UniformityResult result_;
};
}  // namespace uniformity_detail

/// Validates collective control flow, helper stage restrictions and discard ordering.
/// @param module Syntactically and semantically resolved shader module.
/// @param workLimit Maximum expression, statement and dependency-edge visits.
constexpr UniformityResult AnalyzeUniformity(const Module& module, uint32_t workLimit = 1u << 20) {
  bool needed = false;
  for (uint16_t i = 0; i < module.expressionCount; ++i) {
    const Expression& node = module.expressions[i];
    if (node.kind == ExpressionKind::BuiltinCall &&
        (node.payload == uint32_t(Builtin::Fwidth) ||
         node.payload == uint32_t(Builtin::TextureSample)))
      needed = true;
  }
  for (uint16_t i = 0; i < module.statementCount; ++i)
    needed |= module.statements[i].kind == StatementKind::Discard;
  return needed ? uniformity_detail::Analyzer(module, workLimit).run() : UniformityResult{};
}
}  // namespace donner::gpu::shader::wgsl
