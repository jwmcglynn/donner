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

  friend std::ostream& operator<<(std::ostream& os, const QualifiedRule& qualifiedRule) {
    os << "QualifiedRule {\n";
    for (const auto& value : qualifiedRule.prelude) {
      os << "  " << value << "\n";
    }
    os << "  { " << qualifiedRule.block << " }\n";
    return os << "}";
  }
};

struct Rule {
  using Type = std::variant<AtRule, QualifiedRule, InvalidRule>;
  Type value;

  /* implicit */ Rule(Type&& value) : value(value) {}
  bool operator==(const Rule& other) const = default;

  friend std::ostream& operator<<(std::ostream& os, const Rule& rule) {
    std::visit([&os](auto&& v) { os << v; }, rule.value);
    return os;
  }
};

}  // namespace css
}  // namespace donner
