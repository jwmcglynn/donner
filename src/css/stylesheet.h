#pragma once

#include <variant>
#include <vector>

#include "src/css/declaration.h"

namespace donner {
namespace css {

struct QualifiedRule {
  std::vector<ComponentValue> prelude;
  std::optional<SimpleBlock> block;
};

struct Rule {
  using Type = std::variant<AtRule, QualifiedRule>;
  Type value;
};

struct Stylesheet {
  std::vector<Rule> rules;
};

}  // namespace css
}  // namespace donner
