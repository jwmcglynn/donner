#pragma once
/// @file

#include "donner/base/fonts/WoffFont.h"

namespace donner::svg::components {

/**
 * In-memory representation of a font resource, containing the parsed WOFF data.
 */
struct FontResource {
  donner::fonts::WoffFont font;  ///< The parsed WOFF font data.
};

}  // namespace donner::svg::components
