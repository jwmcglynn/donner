#pragma once
/// @file

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>

#include "donner/base/EcsRegistry.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/AllSVGElements.h"

namespace donner::svg::details {

/**
 * Builds SVG elements by tag name. The parser and the incremental source-edit projection in
 * \ref SVGDocument both build elements through it, so a tag produces the same element type, with
 * the same components, either way.
 */
class ElementFactory {
public:
  /**
   * Call `visitor.template operator()<ElementT>()` with the element type that an SVG-namespace tag
   * names.
   *
   * Lookup is exact and case-sensitive. An experimental element type is found only when
   * \p experimentalElementsEnabled is set, as with
   * \ref donner::svg::parser::SVGParser::Options::enableExperimental.
   *
   * @param tagName Local name of the element.
   * @param experimentalElementsEnabled Whether experimental element types are enabled.
   * @param visitor Callable with a `template <typename ElementT> operator()()`.
   * @return True if an element type matched and \p visitor ran.
   */
  template <typename Visitor>
  static bool VisitType(std::string_view tagName, bool experimentalElementsEnabled,
                        Visitor&& visitor) {
    const std::optional<std::size_t> index = FindType(tagName, experimentalElementsEnabled);
    if (!index.has_value()) {
      return false;
    }

    static constexpr auto kVisitors =
        MakeVisitors<std::remove_reference_t<Visitor>>(AllSVGElements{});
    kVisitors[*index](visitor);
    return true;
  }

  /**
   * Create an \p ElementT on \p handle. The entity must hold none of the components that
   * \p ElementT's factory emplaces. Promoting an unknown element in place therefore first removes
   * its own \ref components::ElementTypeComponent, \ref components::TransformComponent and
   * \ref components::RenderingBehaviorComponent.
   *
   * @tparam ElementT Element type from \ref AllSVGElements.
   * @param handle Entity to create the element on.
   */
  template <typename ElementT>
  static ElementT CreateOn(EntityHandle handle) {
    return ElementT::CreateOn(handle);
  }

  /**
   * Create an unknown element on \p handle, for a tag that no enabled element type claims or that
   * is outside the SVG namespace.
   *
   * @param handle Entity to create the element on.
   * @param tagName Qualified tag name of the element.
   */
  static SVGUnknownElement CreateUnknownOn(EntityHandle handle,
                                           const xml::XMLQualifiedNameRef& tagName);

private:
  /**
   * Index in \ref AllSVGElements of the enabled element type that \p tagName names.
   *
   * Two element types claiming the same tag is a build error wherever the lookup table is built as
   * a constant expression, which is every configuration except the one that degrades the table to
   * runtime initialization to stay inside a lower constexpr step budget. There the duplicate
   * instead makes the map fall back to a linear scan that returns the first matching key.
   *
   * @param tagName Local name of the element.
   * @param experimentalElementsEnabled Whether experimental element types are enabled.
   * @return The type index, or \c std::nullopt when no enabled element type claims \p tagName.
   */
  static std::optional<std::size_t> FindType(std::string_view tagName,
                                             bool experimentalElementsEnabled);

  /// Per-type trampolines that call \p Visitor with each type, indexed like \ref AllSVGElements.
  template <typename Visitor, typename... Types>
  static constexpr std::array<void (*)(Visitor&), sizeof...(Types)> MakeVisitors(
      entt::type_list<Types...>) {
    return {[](Visitor& visitor) { visitor.template operator()<Types>(); }...};
  }
};

}  // namespace donner::svg::details
