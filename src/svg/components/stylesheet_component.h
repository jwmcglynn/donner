#pragma once
/// @file

#include "src/css/css.h"
#include "src/svg/properties/presentation_attribute_parsing.h"
#include "src/svg/properties/property_registry.h"

namespace donner::svg::components {

/**
 * Data for a \ref xml_style element.
 *
 * See https://www.w3.org/TR/SVG2/styling.html#StyleElement
 */
struct StylesheetComponent {
  css::Stylesheet stylesheet;
  RcString type;

  /**
   * Returns true if the \ref xml_style element has either no `type` attribute, or if it has been
   * manually set to "text/css".
   */
  bool isCssType() const { return type.empty() || type.equalsIgnoreCase("text/css"); }

  /**
   * Parse the contents of the \ref xml_style element.
   *
   * @param str The contents of the `<style>` element.
   */
  void parseStylesheet(std::string_view str) { stylesheet = css::CSS::ParseStylesheet(str); }
};

}  // namespace donner::svg::components
