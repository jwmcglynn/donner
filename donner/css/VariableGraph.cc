#include "donner/css/VariableGraph.h"

#include <limits>
#include <map>
#include <utility>

#include "donner/base/SmallVector.h"
#include "donner/css/VariableReference.h"

namespace donner::css {
namespace {

using Values = std::span<const ComponentValue>;

constexpr std::size_t kNoDepth = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kMaxNames = 65'536;
constexpr std::size_t kMaxGraphWork = 16 * 1024 * 1024;
constexpr std::size_t kMaxAggregateWork = 16 * 1024 * 1024;

bool ValidateSyntax(Values input, std::size_t& workLeft, bool& syntaxValid) {
  SmallVector<Values, 4> levels;
  levels.emplace_back(input);
  while (!levels.empty()) {
    auto& rest = levels.back();
    if (rest.empty()) {
      levels.pop_back();
      continue;
    }
    if (!workLeft) {
      return false;
    }
    --workLeft;
    const auto& value = rest.front();
    rest = rest.subspan(1);
    if (const auto* function = std::get_if<Function>(&value.value)) {
      if (function->name.equalsLowercase("var") && !details::ParseVariableReference(*function)) {
        syntaxValid = false;
        return true;
      }
      levels.emplace_back(function->values);
    } else if (const auto* block = std::get_if<SimpleBlock>(&value.value)) {
      levels.emplace_back(block->values);
    }
  }
  return true;
}

class SelectedReferenceScanner {
public:
  explicit SelectedReferenceScanner(Values input) { levels_.emplace_back(input); }

  std::optional<details::VariableReference> next(std::size_t& workLeft, bool& refused) {
    while (!levels_.empty()) {
      auto& rest = levels_.back();
      if (rest.empty()) {
        levels_.pop_back();
        continue;
      }
      if (!workLeft) {
        refused = true;
        return std::nullopt;
      }
      --workLeft;
      const auto& value = rest.front();
      rest = rest.subspan(1);
      if (const auto* function = std::get_if<Function>(&value.value)) {
        if (function->name.equalsLowercase("var")) {
          return details::ParseVariableReference(*function);
        }
        levels_.emplace_back(function->values);
      } else if (const auto* block = std::get_if<SimpleBlock>(&value.value)) {
        levels_.emplace_back(block->values);
      }
    }
    return std::nullopt;
  }

  void selectFallback(Values fallback) { levels_.emplace_back(fallback); }

private:
  SmallVector<Values, 4> levels_;
};

enum class NodeState { Unvisited, Visiting, Resolved, Invalid };

struct Node {
  NodeState state = NodeState::Unvisited;
  std::size_t activeDepth = kNoDepth;
  bool syntaxInvalid = false;
  bool cyclic = false;
};

using Names = std::map<std::string_view, std::size_t, std::less<>>;

struct PendingReference {
  std::size_t target;
  std::optional<Values> fallback;
};

struct Frame {
  Frame(std::size_t node, Values values, bool valueInvalid)
      : node(node), scanner(values), valueInvalid(valueInvalid) {}

  std::size_t node;
  SelectedReferenceScanner scanner;
  std::optional<PendingReference> pending;
  bool valueInvalid;
};

void UseInvalidReference(Frame& frame, std::optional<Values> fallback) {
  if (fallback) {
    frame.scanner.selectFallback(*fallback);
  } else {
    frame.valueInvalid = true;
  }
}

class DefinitionResolver {
public:
  DefinitionResolver(std::span<const VariableDefinition> local, const VariableLookup& inherited,
                     const VariableOutputs& outputs, VariableSubstitutionLimits limits,
                     const std::function<bool(std::size_t, std::size_t)>& admit)
      : local_(local),
        inherited_(inherited),
        outputs_(outputs),
        limits_(limits),
        admit_(admit),
        nodes_(local.size()) {}

  bool run() {
    if (local_.size() > kMaxNames || !inherited_ || !outputs_.lookup || !outputs_.publish) {
      return false;
    }
    return collectNames() && validateValues() && evaluate();
  }

private:
  bool collectNames() {
    for (std::size_t i = 0; i < local_.size(); ++i) {
      const auto name = local_[i].name;
      if (name.size() <= 2 || !name.starts_with("--") || !names_.emplace(name, i).second) {
        return false;
      }
    }
    return true;
  }

  bool validateValues() {
    for (std::size_t i = 0; i < local_.size(); ++i) {
      if (!local_[i].value) {
        nodes_[i].syntaxInvalid = true;
        continue;
      }
      bool syntaxValid = true;
      if (!ValidateSyntax(*local_[i].value, validationWorkLeft_, syntaxValid)) {
        return false;
      }
      nodes_[i].syntaxInvalid = !syntaxValid;
    }
    return true;
  }

  bool evaluate() {
    for (std::size_t root = 0; root < local_.size(); ++root) {
      if (nodes_[root].state == NodeState::Unvisited) {
        enter(root);
        if (!evaluateActiveDefinitions()) {
          return false;
        }
      }
    }
    return true;
  }

  bool evaluateActiveDefinitions() {
    while (!frames_.empty()) {
      if (!advance()) {
        return false;
      }
    }
    return true;
  }

  bool advance() {
    if (frames_.back().pending) {
      resumePendingReference();
      return true;
    }
    bool refused = false;
    const auto reference = frames_.back().scanner.next(graphWorkLeft_, refused);
    if (refused) {
      return false;
    }
    return reference ? followReference(*reference) : finishDefinition();
  }

  void resumePendingReference() {
    auto& frame = frames_.back();
    const auto pending = *frame.pending;
    frame.pending.reset();
    if (nodes_[pending.target].state != NodeState::Resolved) {
      UseInvalidReference(frame, pending.fallback);
    }
  }

  bool followReference(const details::VariableReference& reference) {
    const auto found = names_.find(reference.name);
    if (found == names_.end()) {
      if (!inherited_(reference.name)) {
        UseInvalidReference(frames_.back(), reference.fallback);
      }
      return true;
    }

    auto& target = nodes_[found->second];
    switch (target.state) {
      case NodeState::Resolved: return true;
      case NodeState::Invalid: UseInvalidReference(frames_.back(), reference.fallback); return true;
      case NodeState::Visiting:
        if (!markCycle(target.activeDepth)) {
          return false;
        }
        UseInvalidReference(frames_.back(), reference.fallback);
        return true;
      case NodeState::Unvisited:
        frames_.back().pending = PendingReference{found->second, reference.fallback};
        enter(found->second);
        return true;
    }
    return false;
  }

  bool markCycle(std::size_t first) {
    for (std::size_t i = first; i < frames_.size(); ++i) {
      if (!graphWorkLeft_) {
        return false;
      }
      --graphWorkLeft_;
      nodes_[frames_[i].node].cyclic = true;
    }
    return true;
  }

  void enter(std::size_t i) {
    auto& node = nodes_[i];
    node.state = NodeState::Visiting;
    node.activeDepth = frames_.size();
    frames_.emplace_back(i, node.syntaxInvalid ? Values{} : *local_[i].value, node.syntaxInvalid);
  }

  std::optional<VariableSubstitutionResult> computeValue(const Frame& frame) {
    const auto& node = nodes_[frame.node];
    if (frame.valueInvalid || node.cyclic) {
      return VariableSubstitutionResult{VariableSubstitutionStatus::Invalid, {}};
    }
    aggregateRefused_ = false;
    lookupFailure_ = false;
    const VariableLookup lookup = [this](std::string_view name) {
      return lookupComputedValue(name);
    };
    const auto accept = [this](std::size_t work, std::size_t bytes) {
      return acceptAggregate(work, bytes);
    };
    auto value = SubstituteVariables(*local_[frame.node].value, lookup, limits_, accept);
    if (aggregateRefused_ || lookupFailure_) {
      return std::nullopt;
    }
    return value;
  }

  bool finishDefinition() {
    const auto index = frames_.back().node;
    auto value = computeValue(frames_.back());
    if (!value) {
      return false;
    }
    const bool resolved = value->status == VariableSubstitutionStatus::Resolved;
    if (!outputs_.publish(index, std::move(*value))) {
      return false;
    }
    auto& node = nodes_[index];
    node.state = resolved ? NodeState::Resolved : NodeState::Invalid;
    node.activeDepth = kNoDepth;
    frames_.pop_back();
    return true;
  }

  std::optional<Values> lookupComputedValue(std::string_view name) {
    const auto found = names_.find(name);
    if (found == names_.end()) {
      return inherited_(name);
    }
    if (nodes_[found->second].state != NodeState::Resolved) {
      return std::nullopt;
    }
    auto value = outputs_.lookup(found->second);
    lookupFailure_ |= !value.has_value();
    return value;
  }

  bool acceptAggregate(std::size_t work, std::size_t bytes) {
    if (work > componentWorkLeft_ || bytes > byteWorkLeft_ || (admit_ && !admit_(work, bytes))) {
      aggregateRefused_ = true;
      return false;
    }
    componentWorkLeft_ -= work;
    byteWorkLeft_ -= bytes;
    return true;
  }

  std::span<const VariableDefinition> local_;
  const VariableLookup& inherited_;
  const VariableOutputs& outputs_;
  VariableSubstitutionLimits limits_;
  const std::function<bool(std::size_t, std::size_t)>& admit_;
  Names names_;
  std::vector<Node> nodes_;
  SmallVector<Frame, 16> frames_;
  std::size_t validationWorkLeft_ = kMaxGraphWork;
  std::size_t graphWorkLeft_ = kMaxGraphWork;
  std::size_t componentWorkLeft_ = kMaxAggregateWork;
  std::size_t byteWorkLeft_ = kMaxAggregateWork;
  bool aggregateRefused_ = false;
  bool lookupFailure_ = false;
};

}  // namespace

bool ResolveVariableDefinitions(std::span<const VariableDefinition> local,
                                const VariableLookup& inherited, const VariableOutputs& outputs,
                                VariableSubstitutionLimits limits,
                                const std::function<bool(std::size_t, std::size_t)>& admit) {
  return DefinitionResolver(local, inherited, outputs, limits, admit).run();
}

}  // namespace donner::css
