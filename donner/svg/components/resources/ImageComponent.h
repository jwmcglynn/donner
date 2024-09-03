#pragma once
/// @file

#include <optional>

#include "donner/base/RcString.h"
#include "donner/svg/resources/ImageResource.h"

namespace donner::svg::components {

/**
 * Parameters for the \ref xml_image element.
 */
struct ImageComponent {
  /// URI to the image resource, which can either be a file path, URL, or data URL (e.g.
  /// "data:image/png;base64,...").
  RcString href;
};

/**
 * Loaded image resource, created from a \ref ImageComponent.
 */
struct LoadedImageComponent {
  std::optional<ImageResource> image;  //!< Loaded image resource.
};

}  // namespace donner::svg::components
