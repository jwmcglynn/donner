#pragma once
/// @file

#include <any>
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
 * Loaded raster image resource, created from a \ref ImageComponent when the href references a
 * raster image (PNG, JPEG, GIF).
 */
struct LoadedImageComponent {
  std::optional<ImageResource> image;  //!< Loaded image resource.
};

/**
 * Loaded SVG sub-document, created from a \ref ImageComponent when the href references an SVG
 * file (`image/svg+xml`). The sub-document is owned by \ref SubDocumentCache and parsed in
 * \ref ProcessingMode::SecureStatic mode.
 *
 * The document is stored type-erased as `std::any*` to avoid circular build dependencies. The
 * actual type is `SVGDocument`, accessible via `std::any_cast`.
 */
struct LoadedSVGImageComponent {
  /// Non-owning pointer to the cached `std::any` (containing `SVGDocument`).
  /// Owned by \ref SubDocumentCache.
  std::any* subDocument = nullptr;
};

/**
 * Loaded external SVG sub-document referenced by a `<use>` element. When a `<use>` element's
 * href points to an external SVG file, the document is loaded via \ref SubDocumentCache and
 * stored here for rendering as a nested sub-document.
 *
 * The document is stored type-erased as `std::any*` to avoid circular build dependencies.
 * The actual type is `SVGDocument`, accessible via `std::any_cast`.
 */
struct ExternalUseComponent {
  /// Non-owning pointer to the cached `std::any` (containing `SVGDocument`).
  /// Owned by \ref SubDocumentCache.
  std::any* subDocument = nullptr;

  /// Fragment identifier within the external document (e.g., "elementId" from
  /// "file.svg#elementId"). Empty if the whole document is referenced.
  RcString fragment;
};

}  // namespace donner::svg::components
