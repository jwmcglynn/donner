#pragma once
/// @file

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
  Inline,       ///< [DEFAULT] "inline": Causes an element to generate one or more inline boxes.
  Block,        ///< "block": Causes an element to generate a block box.
  ListItem,     ///< "list-item": Causes an element to act as a list item.
  InlineBlock,  ///< "inline-block": Causes an element to generate an inline-level block container.
  Table,        ///< "table": Specifies that an element defines a block-level table, see
                ///< https://www.w3.org/TR/CSS2/tables.html#table-display.
  InlineTable,  ///< "inline-table": Specifies that an element defines a inline-level table, see
                ///< https://www.w3.org/TR/CSS2/tables.html#table-display.
  TableRowGroup,     ///< "table-row-group": Specifies that an element groups one or more rows.
  TableHeaderGroup,  ///< "table-header-group": Like 'table-row-group', but for visual formatting,
                     ///< the row group is always displayed before all other rows and row groups and
                     ///< after any top captions.
  TableFooterGroup,  ///< "table-footer-group": Like 'table-row-group', but for visual formatting,
                     ///< the row group is always displayed after all other rows and row groups and
                     ///< before any bottom captions.
  TableRow,          ///< "table-row": Specifies that an element is a row of cells.
  TableColumnGroup,  ///< "table-column-group": Specifies that an element groups one or more
                     ///< columns.
  TableColumn,       ///< "table-column": Specifies that an element is a column of cells.
  TableCell,         ///< "table-cell": Specifies that an element represents a table cell.
  TableCaption,      ///< "table-caption": Specifies a caption for the table.
  None,              ///< "none": The element is not rendered.
};

/**
 * Ostream output operator for \ref Display enum, outputs the CSS value.
 */
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
 *
 * This determines whether the element is visible or hidden, and whether it affects layout.
 */
enum class Visibility {
  Visible,   ///< [DEFAULT] Visible is the default value.
  Hidden,    ///< Hidden elements are invisible, but still affect layout.
  Collapse,  ///< Collapsed elements are invisible, and do not affect layout.
};

/**
 * Ostream output operator for \ref Visibility enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, Visibility value) {
  switch (value) {
    case Visibility::Visible: return os << "visible";
    case Visibility::Hidden: return os << "hidden";
    case Visibility::Collapse: return os << "collapse";
  }

  UTILS_UNREACHABLE();
}

/**
 * The parsed result of the 'fill-rule' property, see:
 * https://www.w3.org/TR/SVG2/painting.html#FillRuleProperty
 */
enum class FillRule {
  NonZero,  ///< [DEFAULT] Determines "insideness" of a point by counting crossings of a ray drawn
            ///< from that point to infinity and path segments. If crossings is non-zero, the point
            ///< is inside, else outside.
  EvenOdd   ///< Determines "insideness" of a point by counting the number of path segments from the
            ///< shape crossed by a ray drawn from that point to infinity. If count is odd, point is
            ///< inside, else outside.
};

/**
 * Ostream output operator for \ref FillRule enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, FillRule value) {
  switch (value) {
    case FillRule::NonZero: return os << "nonzero";
    case FillRule::EvenOdd: return os << "evenodd";
  }

  UTILS_UNREACHABLE();
}

/**
 * The parsed result of the 'stroke-linecap' property, see:
 * https://www.w3.org/TR/SVG2/painting.html#StrokeLinecapProperty
 */
enum class StrokeLinecap {
  Butt,   ///< [DEFAULT] The stroke is squared off at the endpoint of the path.
  Round,  ///< The stroke is rounded at the endpoint of the path.
  Square  ///< The stroke extends beyond the endpoint of the path by half of the stroke width and
          ///< is squared off.
};

/**
 * Ostream output operator for \ref StrokeLinecap enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, StrokeLinecap value) {
  switch (value) {
    case StrokeLinecap::Butt: return os << "butt";
    case StrokeLinecap::Round: return os << "round";
    case StrokeLinecap::Square: return os << "square";
  }

  UTILS_UNREACHABLE();
}

/**
 * The parsed result of the 'stroke-linejoin' property, see:
 * https://www.w3.org/TR/SVG2/painting.html#StrokeLinejoinProperty
 */
enum class StrokeLinejoin {
  Miter,      ///< [DEFAULT] The outer edges of the strokes for the two segments are extended until
              ///< they meet at an angle, creating a sharp point.
  MiterClip,  ///< Same as miter except the stroke will be clipped if the miter limit is exceeded.
  Round,      ///< The corners of the stroke are rounded off using an arc of a circle with a radius
              ///< equal to the half of the stroke width.
  Bevel,      ///< A triangular shape is used to fill the area between the two stroked segments.
  Arcs  ///< Similar to miter join, but uses an elliptical arc to join the segments, creating a
        ///< smoother joint than miter join when the angle is acute. It is only used for large
        ///< angles where a miter join would be too sharp.
};

/**
 * Ostream output operator for \ref StrokeLinejoin enum, outputs the CSS value.
 */
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

/**
 * The parsed result of the 'stroke-dasharray' property, see:
 * https://www.w3.org/TR/SVG2/painting.html#StrokeDasharrayProperty
 */
using StrokeDasharray = std::vector<Lengthd>;

/**
 * Ostream output operator for \ref StrokeDasharray enum, outputs the CSS value.
 */
inline std::ostream& operator<<(std::ostream& os, const StrokeDasharray& value) {
  for (size_t i = 0; i < value.size(); ++i) {
    if (i > 0) {
      os << ",";
    }
    os << value[i];
  }
  return os;
}

/**
 * Defines how this property cascades between the parent and child elements.
 */
enum class PropertyCascade {
  None,         ///< Property does not inherit.
  Inherit,      ///< Property inherits unconditionally.
  PaintInherit  ///< Property inherits unless the child is instantiated as a paint server. This is
                ///< handled as a special case to prevent recursion for \ref xml_pattern.
};

/**
 * The current property state, which can be either set, not set, or a specific CSS keyword such as
 * "inherit", "initial", or "unset".
 */
enum class PropertyState {
  NotSet = 0,          ///< If the property has no value set.
  Set = 1,             ///< If the property has a value set.
  Inherit = 2,         ///< If the property's value is "inherit".
  ExplicitInitial = 3, /**< If the property's value is "initial", explicitly set by the user.
                        * Sets the property to its initial value with a specificity. */
  ExplicitUnset = 4,   /**< If the property's value is "unset", explicitly set by the user. Resolves
                        *   to either inherit or initial, depending on if the property is inheritable.
                        *
                        * @see https://www.w3.org/TR/css-cascade-3/#inherit-initial
                        */
};

/**
 * Options to control how inheritance is performed, to either inherit everything or conditionally
 * disable inheritance of paint servers.
 */
enum class PropertyInheritOptions {
  All,      ///< Inherit everything (default).
  NoPaint,  ///< Inherit everything except paint servers, for <pattern> elements.
};

/**
 * Callback function to get the initial value of a property.
 *
 * The function returns a `std::optional` to allow for properties that have no initial value.
 */
template <typename T>
using GetInitialFn = std::optional<T> (*)();

/**
 * Holds a CSS property, which has a name and value, and integrates with inheritance to allow
 * cascading values using the CSS model with specificity.
 *
 * @tparam T The type of the property value.
 * @tparam kCascade Determines how this property type participates in the cascade, to allow for
 *   specific property types to be excluded from inheritance.
 */
template <typename T, PropertyCascade kCascade = PropertyCascade::None>
struct Property {
  using Type = T;

  Property(
      std::string_view name,
      GetInitialFn<T> getInitialFn = []() -> std::optional<T> { return std::nullopt; })
      : name(name), getInitialFn(getInitialFn) {}

  /**
   * Get the property value, without considering inheritance. Returns the initial value if
   * the property has not been set.
   *
   * @return The value if it is set, or the initial value if it is not. Returns `std::nullopt` if
   *   the property is none.
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

  /**
   * Clear the current property's value.
   */
  void clear() {
    value.reset();
    state = PropertyState::NotSet;
    specificity = css::Specificity();
  }

  /**
   * Inherit the property from the parent element, if the parent has the property set at a higher
   * specificity.
   *
   * Note that this typically inherits "backwards", taking a local property which may already have a
   * value and then overriding it if the parent has a more specific one.  This is not required, but
   * doing so is more efficient since we don't need to keep setting the property as the child
   * overrides each parent.
   *
   * @param parent Parent property to inherit into this one.
   * @param options Options to control how inheritance is performed, to conditionally disable
   *   inheritance.
   * @return Property with the resolved value after inheritance.
   */
  [[nodiscard]] Property<T, kCascade> inheritFrom(
      const Property<T, kCascade>& parent,
      PropertyInheritOptions options = PropertyInheritOptions::All) const {
    Property<T, kCascade> result = *this;

    if constexpr (kCascade == PropertyCascade::Inherit ||
                  kCascade == PropertyCascade::PaintInherit) {
      assert(parent.state != PropertyState::Inherit && "Parent should already be resolved");

      const bool isPaint = kCascade == PropertyCascade::PaintInherit;
      const bool canInherit = options == PropertyInheritOptions::All ||
                              (options == PropertyInheritOptions::NoPaint && !isPaint);

      if (parent.hasValue() && canInherit) {
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

  /**
   * Convert the units of this property to pixel-relative values, if it contains a value which is
   * relative such as a font- or viewport-relative length.
   *
   * @param viewbox The viewbox to use for resolving relative lengths.
   * @param fontMetrics The font metrics to use for resolving relative lengths.
   */
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

  std::string_view name;                        ///< Property name, such as "color".
  std::optional<T> value;                       ///< Property value, or `std::nullopt` if not set.
  PropertyState state = PropertyState::NotSet;  ///< Current state of the property, such as set or
                                                ///< inherited.
  css::Specificity specificity;  ///< Specificity of the property, used for inheritance.

  GetInitialFn<T>
      getInitialFn;  ///< Function which is called to get the initial value of the property.
};

/**
 * Ostream output operator, which outputs the current property value, how it was set (e.g. directly
 * set or inherited, see \ref PropertyState), and the property's specificity.
 *
 * Example output:
 * ```
 * color: Color(0, 255, 0, 255) (set) @ Specificity(0, 0, 0)
 * ```
 *
 * @param os Output stream to write to.
 * @param property Property to output.
 */
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
