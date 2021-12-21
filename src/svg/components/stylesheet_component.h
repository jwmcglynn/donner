#pragma once

#include "src/css/parser/stylesheet_parser.h"
#include "src/svg/properties/property_registry.h"

namespace donner {

struct StylesheetComponent {
  css::Stylesheet stylesheet;
  RcString type;

  bool isCssType() const { return type.empty() || type.equalsIgnoreCase("text/css"); }

  /**
   * Parse the contents of the <style> element.
   */
  void parseStylesheet(std::string_view str) { stylesheet = css::StylesheetParser::Parse(str); }
};

}  // namespace donner
