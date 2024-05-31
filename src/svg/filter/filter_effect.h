#pragma once
/// @file

#include <variant>

#include "src/base/length.h"
#include "src/svg/graph/reference.h"

namespace donner::svg {

struct FilterEffect {
  struct None {
    bool operator==(const None&) const = default;
  };

  struct Blur {
    Lengthd stdDeviation;

    bool operator==(const Blur&) const = default;
  };

  struct ElementReference {
    Reference reference;

    explicit ElementReference(Reference reference) : reference(std::move(reference)) {}
    bool operator==(const ElementReference&) const = default;
  };

  /**
   * Variant containing all supported effects.
   */
  using Type = std::variant<None, Blur, ElementReference>;
  Type value;

  /* implicit */ FilterEffect(Type value) : value(std::move(value)) {}

  // Allow constexpr construction for None.
  /* implicit */ constexpr FilterEffect(None) : value(None{}) {}

  bool operator==(const FilterEffect& other) const;

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

  friend std::ostream& operator<<(std::ostream& os, const FilterEffect& filter);
};

}  // namespace donner::svg
