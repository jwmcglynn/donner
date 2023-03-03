#pragma once
/// @file

#include <iostream>
#include <optional>
#include <variant>
#include <vector>

#include "src/base/rc_string.h"
#include "src/css/rule.h"

namespace donner::css {

struct Declaration {
  Declaration(RcString name, std::vector<ComponentValue> values = {}, size_t sourceOffset = 0,
              bool important = false)
      : name(std::move(name)),
        values(std::move(values)),
        sourceOffset(sourceOffset),
        important(important) {}

  bool operator==(const Declaration& other) const = default;

  friend std::ostream& operator<<(std::ostream& os, const Declaration& declaration);

  RcString name;
  std::vector<ComponentValue> values;
  size_t sourceOffset;
  bool important = false;
};

struct DeclarationOrAtRule {
  using Type = std::variant<Declaration, AtRule, InvalidRule>;
  Type value;

  /* implicit */ DeclarationOrAtRule(Type&& value);
  bool operator==(const DeclarationOrAtRule& other) const;

  friend std::ostream& operator<<(std::ostream& os, const DeclarationOrAtRule& declOrAt) {
    std::visit([&os](auto&& v) { os << v; }, declOrAt.value);
    return os;
  }
};

}  // namespace donner::css
