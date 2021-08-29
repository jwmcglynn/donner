#pragma once

#include <iostream>
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
  size_t sourceOffset;

  Function(std::string name, size_t sourceOffset);
  bool operator==(const Function& other) const;

  friend std::ostream& operator<<(std::ostream& os, const Function& func);
};

struct SimpleBlock {
  TokenIndex associatedToken;
  std::vector<ComponentValue> values;
  size_t sourceOffset;

  explicit SimpleBlock(TokenIndex associatedToken, size_t sourceOffset);
  bool operator==(const SimpleBlock& other) const;

  friend std::ostream& operator<<(std::ostream& os, const SimpleBlock& block);
};

struct ComponentValue {
  using Type = std::variant<Token, Function, SimpleBlock>;
  Type value;

  /* implicit */ ComponentValue(Type&& value);
  bool operator==(const ComponentValue& other) const;

  template <typename T>
  bool is() const {
    return std::holds_alternative<T>(value);
  }

  template <typename T>
  T& get() & {
    return std::get<indexOf<T>()>(value);
  }

  template <typename T>
  const T& get() const& {
    return std::get<indexOf<T>()>(value);
  }

  template <typename T>
  T&& get() && {
    return std::move(std::get<indexOf<T>()>(value));
  }

  template <typename T, TokenIndex index = 0>
  static constexpr TokenIndex indexOf() {
    if constexpr (index == std::variant_size_v<Type>) {
      return index;
    } else if constexpr (std::is_same_v<std::variant_alternative_t<index, Type>, T>) {
      return index;
    } else {
      return indexOf<T, index + 1>();
    }
  }

  size_t sourceOffset() const {
    return std::visit(
        [](auto&& v) -> size_t {
          using T = std::remove_cvref_t<decltype(v)>;

          if constexpr (std::is_same_v<Token, T>) {
            return v.offset();
          } else {
            return v.sourceOffset;
          }
        },
        value);
  }

  friend std::ostream& operator<<(std::ostream& os, const ComponentValue& component) {
    std::visit([&os](auto&& v) { os << v; }, component.value);
    return os;
  }
};

struct AtRule {
  std::string name;
  std::vector<ComponentValue> prelude;
  std::optional<SimpleBlock> block;

  explicit AtRule(std::string name);
  bool operator==(const AtRule& other) const;
};

struct Declaration {
  Declaration(std::string name, std::vector<ComponentValue> values = {}, size_t sourceOffset = 0,
              bool important = false)
      : name(std::move(name)),
        values(std::move(values)),
        sourceOffset(sourceOffset),
        important(important) {}

  bool operator==(const Declaration& other) const = default;

  std::string name;
  std::vector<ComponentValue> values;
  size_t sourceOffset;
  bool important = false;
};

struct InvalidRule {
  enum class Type { Default, ExtraInput };

  explicit InvalidRule(Type type = Type::Default) : type(type) {}
  bool operator==(const InvalidRule& other) const { return type == other.type; }

  Type type;
};

struct DeclarationOrAtRule {
  using Type = std::variant<Declaration, AtRule, InvalidRule>;
  Type value;

  /* implicit */ DeclarationOrAtRule(Type&& value);
  bool operator==(const DeclarationOrAtRule& other) const;
};

}  // namespace css
}  // namespace donner
