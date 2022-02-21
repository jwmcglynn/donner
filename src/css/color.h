#pragma once

#include <cassert>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "src/css/declaration.h"

namespace donner::css {

struct RGBA {
  uint8_t r = 0xFF;
  uint8_t g = 0xFF;
  uint8_t b = 0xFF;
  uint8_t a = 0xFF;

  constexpr RGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) : r(r), g(g), b(b), a(a) {}

  static constexpr RGBA RGB(uint8_t r, uint8_t g, uint8_t b) { return {r, g, b, 0xFF}; }

  bool operator==(const RGBA&) const = default;
};

struct Color {
  struct CurrentColor {
    bool operator==(const CurrentColor&) const = default;
  };

  using Type = std::variant<RGBA, CurrentColor>;
  Type value;

  /* implicit */ constexpr Color(Type value) : value(std::move(value)) {}

  bool operator==(const Color& other) const;

  static std::optional<Color> ByName(std::string_view name);

  bool isCurrentColor() const { return std::holds_alternative<CurrentColor>(value); }
  bool hasRGBA() const { return std::holds_alternative<RGBA>(value); }

  RGBA rgba() const { return std::get<RGBA>(value); }

  RGBA resolve(RGBA currentColor, float opacity) const {
    RGBA value = isCurrentColor() ? currentColor : rgba();
    if (opacity != 1.0f) {
      value.a = static_cast<uint8_t>(value.a * opacity);
    }
    return value;
  }

  friend std::ostream& operator<<(std::ostream& os, const Color& color);
};

namespace string_literals {

constexpr Color operator"" _rgb(unsigned long long value) {
  return Color(RGBA::RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF));
}

constexpr Color operator"" _rgba(unsigned long long value) {
  return Color(RGBA((value >> 24) & 0xFF, (value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF));
}

}  // namespace string_literals

}  // namespace donner::css
