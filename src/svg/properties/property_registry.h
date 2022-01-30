#pragma once

#include <span>

#include "src/base/box.h"
#include "src/base/parser/parse_result.h"
#include "src/css/color.h"
#include "src/css/declaration.h"
#include "src/css/stylesheet.h"
#include "src/svg/properties/paint_server.h"

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

class PropertyRegistry;

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

enum class PropertyCascade { None, Inherit };

struct PropertyParseFnParams {
  std::span<const css::ComponentValue> components;
  PropertyState explicitState = PropertyState::NotSet;
  css::Specificity specificity;
  /// For presentation attributes, values may be unitless, in which case they the spec says they are
  /// specified in "user units". See https://www.w3.org/TR/SVG2/types.html#syntax.
  bool allowUserUnits = false;
};

using PropertyParseFn = std::optional<ParseError> (*)(PropertyRegistry& registry,
                                                      const PropertyParseFnParams& params);

class PropertyRegistry {
public:
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

  Property<css::Color, PropertyCascade::Inherit> color{"color"};
  Property<PaintServer, PropertyCascade::Inherit> fill{
      "fill", []() -> std::optional<PaintServer> {
        return PaintServer::Solid(css::Color(css::RGBA::RGB(0, 0, 0)));
      }};

  // Stroke.
  Property<PaintServer, PropertyCascade::Inherit> stroke{
      "stroke", []() -> std::optional<PaintServer> { return PaintServer::None(); }};
  Property<double, PropertyCascade::Inherit> strokeOpacity{
      "stroke-opacity", []() -> std::optional<double> { return 1.0; }};
  Property<Lengthd, PropertyCascade::Inherit> strokeWidth{
      "stroke-width", []() -> std::optional<Lengthd> { return Lengthd(1, Lengthd::Unit::None); }};
  Property<StrokeLinecap, PropertyCascade::Inherit> strokeLinecap{
      "stroke-linecap", []() -> std::optional<StrokeLinecap> { return StrokeLinecap::Butt; }};
  Property<StrokeLinejoin, PropertyCascade::Inherit> strokeLinejoin{
      "stroke-linejoin", []() -> std::optional<StrokeLinejoin> { return StrokeLinejoin::Miter; }};
  Property<double, PropertyCascade::Inherit> strokeMiterlimit{
      "stroke-miterlimit", []() -> std::optional<double> { return 4.0; }};
  Property<StrokeDasharray, PropertyCascade::Inherit> strokeDasharray{
      "stroke-dasharray", []() -> std::optional<StrokeDasharray> { return std::nullopt; }};
  Property<Lengthd, PropertyCascade::Inherit> strokeDashoffset{
      "stroke-dashoffset",
      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};

  /**
   * Return a tuple of all properties within the PropertyRegistry.
   */
  auto allProperties() {
    return std::forward_as_tuple(color, fill, stroke, strokeOpacity, strokeWidth, strokeLinecap,
                                 strokeLinejoin, strokeMiterlimit, strokeDasharray,
                                 strokeDashoffset);
  }

  static constexpr size_t numProperties() {
    // If this is at class scope, it fails with a compiler error: "function with deduced return type
    // cannot be used before it is defined".
    using PropertiesTuple =
        std::invoke_result_t<decltype(&PropertyRegistry::allProperties), PropertyRegistry>;
    return std::tuple_size_v<PropertiesTuple>;
  }

  template <size_t Start, size_t End, class F>
  static constexpr void forEachProperty(F&& f) {
    if constexpr (Start < End) {
      f(std::integral_constant<size_t, Start>{});
      forEachProperty<Start + 1, End>(f);
    }
  }

  /**
   * Inherit the value of each element in the stylesheet.
   */
  [[nodiscard]] PropertyRegistry inheritFrom(const PropertyRegistry& parent) const {
    PropertyRegistry result;
    auto resultProperties = result.allProperties();
    const auto parentProperties = const_cast<PropertyRegistry&>(parent).allProperties();
    const auto selfProperties = const_cast<PropertyRegistry*>(this)->allProperties();

    forEachProperty<0, numProperties()>(
        [&resultProperties, parentProperties, selfProperties](auto i) {
          std::get<i.value>(resultProperties) =
              std::get<i.value>(selfProperties).inheritFrom(std::get<i.value>(parentProperties));
        });

    return result;
  }

  void resolveUnits(const Boxd& viewbox, const FontMetrics& fontMetrics) {
    std::apply([&viewbox, &fontMetrics](
                   auto&&... property) { (property.resolveUnits(viewbox, fontMetrics), ...); },
               allProperties());
  }

  /**
   * Parse a single declaration, adding it to the property registry.
   *
   * @param declaration Declaration to parse.
   * @param specificity Specificity of the declaration.
   * @return Error if the declaration had errors parsing or the property is not supported.
   */
  std::optional<ParseError> parseProperty(const css::Declaration& declaration,
                                          css::Specificity specificity);

  /**
   * Parse a HTML/SVG style attribute, corresponding to a CSS <declaration-list>, ignoring any parse
   * errors or unsupported properties.
   *
   * @param str Input string.
   */
  void parseStyle(std::string_view str);

  /**
   * Parse a presentation attribute, which can contain a CSS value.
   *
   * @see https://www.w3.org/TR/SVG2/styling.html#PresentationAttributes
   * @param name Name of the attribute.
   * @param value Value of the attribute, parsed as a CSS value.
   * @return true if the attribute name was supported.
   */
  bool parsePresentationAttribute(std::string_view name, std::string_view value);

  friend std::ostream& operator<<(std::ostream& os, const PropertyRegistry& registry);
};

#if 1
template <typename T, PropertyCascade kCascade>
std::ostream& operator<<(std::ostream& os,
                         const PropertyRegistry::Property<T, kCascade>& property) {
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
#endif

}  // namespace donner::svg
