#pragma once

#include "src/base/box.h"
#include "src/base/parser/parse_result.h"
#include "src/css/declaration.h"

namespace donner::svg {

enum class StrokeLinecap { Butt, Round, Square };

inline std::ostream& operator<<(std::ostream& os, StrokeLinecap value) {
  switch (value) {
    case StrokeLinecap::Butt: return os << "butt";
    case StrokeLinecap::Round: return os << "round";
    case StrokeLinecap::Square: return os << "square";
  }

  UTILS_UNREACHABLE();
}

enum class StrokeLinejoin { Miter, MiterClip, Round, Bevel, Arcs };

inline std::ostream& operator<<(std::ostream& os, StrokeLinejoin value) {
  switch (value) {
    case StrokeLinejoin::Miter: return os << "miter";
    case StrokeLinejoin::MiterClip: return os << "miter-clip";
    case StrokeLinejoin::Round: return os << "round";
    case StrokeLinejoin::Bevel: return os << "bevel";
    case StrokeLinejoin::Arcs: return os << "arcs";
  }

  UTILS_UNREACHABLE();
}

using StrokeDasharray = std::vector<Lengthd>;

inline std::ostream& operator<<(std::ostream& os, const StrokeDasharray& value) {
  for (size_t i = 0; i < value.size(); ++i) {
    if (i > 0) {
      os << ",";
    }
    os << value[i];
  }
  return os;
}

enum class PropertyCascade { None, Inherit };

enum class PropertyState {
  NotSet = 0,
  Set = 1,             //!< If the property has a value set.
  Inherit = 2,         //!< If the property's value is "inherit".
  ExplicitInitial = 3, /**< If the property's value is "initial", explicitly set by the user. Sets
                        *   the property to its initial value with a specificity. */
  ExplicitUnset = 4,   /**< If the property's value is "unset", explicitly set by the user. Resolves
                        *   to either inherit or initial, depending on if the property is inheritable.
                        *
                        * @see https://www.w3.org/TR/css-cascade-3/#inherit-initial
                        */
};

template <typename T>
using GetInitialFn = std::optional<T> (*)();

template <typename T, PropertyCascade kCascade = PropertyCascade::None>
struct Property {
  using Type = T;

  Property(
      std::string_view name,
      GetInitialFn<T> getInitialFn = []() -> std::optional<T> { return std::nullopt; })
      : name(name), getInitialFn(getInitialFn) {}

  /**
   * Get the property value, without considering inheritance. Returns the initial value if the
   * property has not been set.
   */
  std::optional<T> get() const { return state == PropertyState::Set ? value : getInitialFn(); }
  T getRequired() const {
    auto result = get();
    UTILS_RELEASE_ASSERT_MSG(result.has_value(), "Required property not set");
    return std::move(result.value());
  }

  void set(std::optional<T> newValue, css::Specificity newSpecificity) {
    value = std::move(newValue);
    state = PropertyState::Set;
    specificity = newSpecificity;
  }

  void set(PropertyState newState, css::Specificity newSpecificity) {
    value.reset();
    state = newState;
    specificity = newSpecificity;
  }

  [[nodiscard]] Property<T, kCascade> inheritFrom(const Property<T, kCascade>& parent) const {
    Property<T, kCascade> result = *this;

    if constexpr (kCascade == PropertyCascade::Inherit) {
      assert(parent.state != PropertyState::Inherit && "Parent should already be resolved");

      if (parent.hasValue()) {
        if (state == PropertyState::NotSet || state == PropertyState::Inherit ||
            state == PropertyState::ExplicitUnset) {
          // Inherit from parent.
          result.value = parent.get();
          // Keep current specificity.
          result.state = PropertyState::Set;
        } else if (parent.specificity > specificity) {
          // Inherit from parent, but with a lower specificity.
          result.value = parent.get();
          result.specificity = parent.specificity;
          result.state = PropertyState::Set;
        }
      }
    } else {
      // Inherit only if the state is Inherit.
      if (state == PropertyState::Inherit) {
        result.value = parent.get();
        // Keep current specificity.
        result.state = PropertyState::Set;
      }
    }

    return result;
  }

  void resolveUnits(const Boxd& viewbox, const FontMetrics& fontMetrics) {
    if constexpr (std::is_same_v<Lengthd, Type>) {
      if (value) {
        value = Lengthd(value->toPixels(viewbox, fontMetrics), Lengthd::Unit::Px);
      }
    }
  }

  /**
   * @return true if the property has any value set, including CSS built-in values.
   */
  bool hasValue() const { return state != PropertyState::NotSet; }

  std::string_view name;
  std::optional<T> value;
  PropertyState state = PropertyState::NotSet;
  css::Specificity specificity;

  GetInitialFn<T> getInitialFn;
};

template <typename T, PropertyCascade kCascade>
std::ostream& operator<<(std::ostream& os, const Property<T, kCascade>& property) {
  os << property.name << ":";

  if (property.state == PropertyState::Set) {
    if (property.value) {
      os << " " << *property.value;
    } else {
      os << " nullopt";
    }
  }

  switch (property.state) {
    case PropertyState::Set: os << " (set)"; break;
    case PropertyState::Inherit: os << " (inherit)"; break;
    case PropertyState::ExplicitInitial: os << " (explicit initial)"; break;
    case PropertyState::ExplicitUnset: os << " (explicit unset)"; break;
    case PropertyState::NotSet: os << " (not set)"; break;
  }

  if (property.state != PropertyState::NotSet) {
    os << " @ " << property.specificity;
  }

  return os;
}

}  // namespace donner::svg