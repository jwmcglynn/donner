#pragma once

#include <variant>
#include <vector>

#include "src/css/declaration.h"

namespace donner {
namespace css {

struct QualifiedRule {
  std::vector<ComponentValue> prelude;
  SimpleBlock block;

  QualifiedRule(std::vector<ComponentValue>&& prelude, SimpleBlock&& block)
      : prelude(prelude), block(block) {}
  bool operator==(const QualifiedRule& other) const = default;
};

struct Rule {
  using Type = std::variant<AtRule, QualifiedRule, InvalidRule>;
  Type value;

  /* implicit */ Rule(Type&& value) : value(value) {}
  bool operator==(const Rule& other) const = default;
};

}  // namespace css
}  // namespace donner
