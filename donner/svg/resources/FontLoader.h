#pragma once
/// @file

#include "donner/base/RcString.h"
#include "donner/svg/components/resources/FontResource.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"
#include "donner/svg/resources/UrlLoader.h"

namespace donner::svg {

/**
 * Helper to load a font from a URI, using a \ref ResourceLoaderInterface to fetch the raw data and
 * then parsing it as a WOFF file.
 */
class FontLoader {
public:
  /**
   * @param loader The user-provided resource loader to fetch the URI.
   */
  explicit FontLoader(ResourceLoaderInterface& loader);

  /**
   * Load a font from a URI.
   *
   * @param uri The URI to load, which can be a file path or a data URI.
   * @return The loaded font resource, or an error if loading failed.
   */
  std::variant<components::FontResource, UrlLoaderError> fromUri(const RcString& uri);

  /**
   * Load a font from raw data.
   *
   * @param data The raw data to load.
   * @return The loaded font resource, or an error if loading failed.
   */
  std::variant<components::FontResource, UrlLoaderError> fromData(std::span<const uint8_t> data);

private:
  UrlLoader urlLoader_;
};

}  // namespace donner::svg
