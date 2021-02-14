#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "src/base/length.h"
#include "src/css/token.h"

namespace donner {
namespace css {

struct ComponentValue;

struct Function {
  std::string name;
  std::vector<ComponentValue> values;

  explicit Function(std::string name);
  bool operator==(const Function& other) const;
};

struct SimpleBlock {
  TokenIndex associatedToken;
  std::vector<ComponentValue> values;

  explicit SimpleBlock(TokenIndex associatedToken);
  bool operator==(const SimpleBlock& other) const;
};

struct ComponentValue {
  using Type = std::variant<Token, Function, SimpleBlock>;
  Type value;

  /* implicit */ ComponentValue(Type&& value);
  bool operator==(const ComponentValue& other) const;
};

struct AtRule {
  std::string name;
  std::vector<ComponentValue> prelude;
  std::optional<SimpleBlock> block;

  explicit AtRule(std::string name);
  bool operator==(const AtRule& other) const;
};

struct Declaration {
  Declaration(std::string name, std::vector<ComponentValue> values = {}, bool important = false)
      : name(std::move(name)), values(std::move(values)), important(important) {}

  bool operator==(const Declaration& other) const = default;

  std::string name;
  std::vector<ComponentValue> values;
  bool important = false;
};

struct InvalidRule {
  bool operator==(const InvalidRule& other) const { return true; }
};

struct DeclarationOrAtRule {
  using Type = std::variant<Declaration, AtRule, InvalidRule>;
  Type value;

  /* implicit */ DeclarationOrAtRule(Type&& value);
  bool operator==(const DeclarationOrAtRule& other) const;
};

}  // namespace css
}  // namespace donner
