#pragma once

#include <cassert>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "src/base/rc_string.h"
#include "src/css/color.h"

namespace donner::svg {

struct PaintServer {
  struct None {
    bool operator==(const None&) const = default;
  };

  struct ContextFill {
    bool operator==(const ContextFill&) const = default;
  };

  struct ContextStroke {
    bool operator==(const ContextStroke&) const = default;
  };

  struct Solid {
    css::Color color;

    constexpr explicit Solid(css::Color color) : color(std::move(color)) {}
    bool operator==(const Solid&) const = default;
  };

  struct Reference {
    RcString url;
    std::optional<css::Color> fallback;

    Reference(RcString url, std::optional<css::Color> fallback = std::nullopt)
        : url(std::move(url)), fallback(std::move(fallback)) {}
    bool operator==(const Reference&) const = default;
  };

  using Type = std::variant<None, ContextFill, ContextStroke, Solid, Reference>;
  Type value;

  /* implicit */ PaintServer(Type value) : value(std::move(value)) {}

  bool operator==(const PaintServer& other) const;

  template <typename T>
  bool is() const {
    return std::holds_alternative<T>(value);
  }

  template <typename T>
  T& get() & {
    return std::get<T>(value);
  }

  template <typename T>
  const T& get() const& {
    return std::get<T>(value);
  }

  template <typename T>
  T&& get() && {
    return std::move(std::get<T>(value));
  }

  friend std::ostream& operator<<(std::ostream& os, const PaintServer& paint);
};

}  // namespace donner::svg