#pragma once

#include <compare>
#include <cstdint>
#include <ostream>

#include "src/css/declaration.h"
#include "src/css/selector.h"

namespace donner {
namespace css {

struct SelectorRule {
  Selector selector;
  std::vector<Declaration> declarations;
};

class Stylesheet {
public:
  Stylesheet() = default;
  explicit Stylesheet(std::vector<SelectorRule>&& rules) : rules_(std::move(rules)) {}

  Stylesheet(Stylesheet&&) = default;
  Stylesheet& operator=(Stylesheet&&) = default;

  std::span<const SelectorRule> rules() const { return rules_; }

private:
  std::vector<SelectorRule> rules_;
};

}  // namespace css
}  // namespace donner
