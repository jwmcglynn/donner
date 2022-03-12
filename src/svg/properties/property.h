#pragma once

#include "src/base/box.h"
#include "src/base/parser/parse_result.h"
#include "src/css/declaration.h"
#include "src/css/specificity.h"

namespace donner::svg {

/**
 * The parsed result of the CSS 'display' property, see
 * https://www.w3.org/TR/CSS2/visuren.html#propdef-display.
 *
 * Note that in SVG2, there are only two distinct behaviors, 'none', and everything else rendered as
 * normal, see https://www.w3.org/TR/SVG2/render.html#VisibilityControl
 *
 * > Elements that have any other display value than none are rendered as normal.
 *
 */
enum class Display {
  Inline,  //!< Inline is the default value.
  Block,
  ListItem,
  InlineBlock,
  Table,
  InlineTable,
  TableRowGroup,
  TableHeaderGroup,
  TableFooterGroup,
  TableRow,
  TableColumnGroup,
  TableColumn,
  TableCell,
  TableCaption,
  None,
};

inline std::ostream& operator<<(std::ostream& os, Display value) {
  switch (value) {
    case Display::Inline: return os << "inline";
    case Display::Block: return os << "block";
    case Display::ListItem: return os << "list-item";
    case Display::InlineBlock: return os << "inline-block";
    case Display::Table: return os << "table";
    case Display::InlineTable: return os << "inline-table";
    case Display::TableRowGroup: return os << "table-row-group";
    case Display::TableHeaderGroup: return os << "table-header-group";
    case Display::TableFooterGroup: return os << "table-footer-group";
    case Display::TableRow: return os << "table-row";
    case Display::TableColumnGroup: return os << "table-column-group";
    case Display::TableColumn: return os << "table-column";
    case Display::TableCell: return os << "table-cell";
    case Display::TableCaption: return os << "table-caption";
    case Display::None: return os << "none";
  }

  UTILS_UNREACHABLE();
}

/**
 * The parsed result of the 'visibility' property, see:
 * https://www.w3.org/TR/CSS2/visufx.html#propdef-visibility
 */
enum class Visibility {
  Visible,  //!< Visible is the default value.
  Hidden,
  Collapse,
};

inline std::ostream& operator<<(std::ostream& os, Visibility value) {
  switch (value) {
    case Visibility::Visible: return os << "visible";
    case Visibility::Hidden: return os << "hidden";
    case Visibility::Collapse: return os << "collapse";
  }

  UTILS_UNREACHABLE();
}

enum class FillRule { NonZero, EvenOdd };

inline std::ostream& operator<<(std::ostream& os, FillRule value) {
  switch (value) {
    case FillRule::NonZero: return os << "nonzero";
    case FillRule::EvenOdd: return os << "evenodd";
  }

  UTILS_UNREACHABLE();
}

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
   *
   * @return The value if it is set, or the initial value if it is not. Returns std::nullopt if the
   *  property is none.
   */
  std::optional<T> get() const { return state == PropertyState::Set ? value : getInitialFn(); }

  /**
   * Gets the value of the property, requiring that the value is not std::nullopt.
   *
   * @return The value.
   */
  T getRequired() const {
    auto result = get();
    UTILS_RELEASE_ASSERT_MSG(result.has_value(), "Required property not set");
    return std::move(result.value());
  }

  /**
   * Gets a const-ref to the value, for accessing complex types without copying. Requires that \ref
   * hasValue() is true.
   *
   * @return const T& Reference to the value.
   */
  const T& getRequiredRef() const {
    UTILS_RELEASE_ASSERT_MSG(hasValue(), "Required property not set");
    return value.value();
  }

  /**
   * Set the property to a new value at the given specificity.
   *
   * @param newValue Value to set, or std::nullopt to set to an empty value.
   * @param newSpecificity Specificity to use.
   */
  void set(std::optional<T> newValue, css::Specificity newSpecificity) {
    value = std::move(newValue);
    state = PropertyState::Set;
    specificity = newSpecificity;
  }

  /**
   * Unset the current value and set the property to a specific state.
   *
   * @param newState New state to set.
   * @param newSpecificity Specificity to use.
   */
  void set(PropertyState newState, css::Specificity newSpecificity) {
    value.reset();
    state = newState;
    specificity = newSpecificity;
  }

  /**
   * Replace the current property's value with a new value at the current specificity.
   *
   * @param newValue Value to use to replace the existing one.
   */
  void substitute(std::optional<T> newValue) {
    value = std::move(newValue);
    state = PropertyState::Set;
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
