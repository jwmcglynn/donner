#pragma once

#include "src/svg/properties/property_registry.h"

namespace donner {

struct StyleComponent {
  svg::PropertyRegistry properties;

  void setStyle(std::string_view style) { properties.parseStyle(style); }
  bool trySetPresentationAttribute(std::string_view name, std::string_view value) {
    return properties.parsePresentationAttribute(name, value);
  }
};

}  // namespace donner