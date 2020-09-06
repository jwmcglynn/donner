#pragma once

#include <string>
#include <variant>
#include <vector>

#include "src/base/length.h"

namespace donner {
namespace css {

struct Color {
  Color() : Color(0, 0, 0) {}
  Color(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b) {}
  Color(uint8_t a, uint8_t r, uint8_t g, uint8_t b) : a(a), r(r), g(g), b(b) {}

  uint8_t a = 0xFF;
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

struct Function {
  std::string name;
  std::string args;
};

struct Declaration {
  // todo
  // string
  // color
  // function

  class Value {
    std::variant<std::string, Color, Function> value_;
  };

  std::string name_;
  std::vector<Value> values_;
  bool important_ = false;
};

}  // namespace css
}  // namespace donner
