#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "src/base/length.h"
#include "src/css/token.h"

namespace donner {
namespace css {

struct SimpleBlock;
struct Function;
using ComponentValue = std::variant<Token, Function, SimpleBlock>;

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

struct AtRule {
  std::string name;
  std::vector<ComponentValue> prelude;
  std::optional<ComponentValue> block;

  bool operator==(const AtRule& other) const = default;
};

struct Declaration {
  Declaration(std::string name, std::vector<ComponentValue> values = {}, bool important = false)
      : name(std::move(name)), values(std::move(values)), important(important) {}

  bool operator==(const Declaration& other) const = default;

  std::string name;
  std::vector<ComponentValue> values;
  bool important = false;
};

using DeclarationOrAtRule = std::variant<Declaration, AtRule>;

}  // namespace css
}  // namespace donner
