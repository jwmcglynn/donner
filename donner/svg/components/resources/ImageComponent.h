#pragma once
/// @file

#include <atomic>
#include <cstdint>
#include <optional>

#include "donner/base/RcString.h"
#include "donner/svg/SVGDocumentHandle.h"
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
  LoadedImageComponent() : revision_(NextRevision()) {}
  explicit LoadedImageComponent(ImageResource loadedImage)
      : image(std::move(loadedImage)), revision_(NextRevision()) {}

  /// Monotonic identity for the current decoded image payload.
  std::uint64_t revision() const { return revision_; }

  /// Mark a direct mutation of the decoded image payload.
  void markImageModified() { revision_ = NextRevision(); }

  std::optional<ImageResource> image;  //!< Loaded image resource.

private:
  static std::uint64_t NextRevision() {
    static std::atomic<std::uint64_t> nextRevision{1};
    return nextRevision.fetch_add(1, std::memory_order_relaxed);
  }

  std::uint64_t revision_ = 0;
};

/**
 * Loaded SVG sub-document, created from a \ref ImageComponent when the href references an SVG
 * file (`image/svg+xml`). Stores the shared internal document handle used by \ref SVGDocument.
 */
struct LoadedSVGImageComponent {
  SVGDocumentHandle subDocument;  //!< Parsed external SVG sub-document handle.
};

/**
 * Loaded external SVG sub-document referenced by a `<use>` element. When a `<use>` element's
 * href points to an external SVG file, the document is loaded via \ref SubDocumentCache and
 * stored here as an \ref SVGDocumentHandle for rendering as a nested sub-document.
 */
struct ExternalUseComponent {
  SVGDocumentHandle subDocument;  //!< Parsed external SVG sub-document handle.

  /// Fragment identifier within the external document (e.g., "elementId" from
  /// "file.svg#elementId"). Empty if the whole document is referenced.
  RcString fragment;
};

}  // namespace donner::svg::components
