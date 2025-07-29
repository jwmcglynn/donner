#include "donner/svg/resources/FontLoader.h"

#include "donner/base/fonts/WoffParser.h"

namespace donner::svg {

FontLoader::FontLoader(ResourceLoaderInterface& loader) : urlLoader_(loader) {}

std::variant<components::FontResource, UrlLoaderError> FontLoader::fromUri(const RcString& uri) {
  auto loadedResource = urlLoader_.fromUri(uri);
  if (auto* error = std::get_if<UrlLoaderError>(&loadedResource)) {
    return *error;
  }

  auto woffResult = std::get<UrlLoader::Result>(std::move(loadedResource));

  auto maybeFont = fonts::WoffParser::Parse(woffResult.data);
  if (maybeFont.hasError()) {
    return UrlLoaderError::DataCorrupt;
  }

  return components::FontResource{std::move(maybeFont.result())};
}

std::variant<components::FontResource, UrlLoaderError> FontLoader::fromData(
    std::span<const uint8_t> data) {
  auto maybeFont = fonts::WoffParser::Parse(data);
  if (maybeFont.hasError()) {
    return UrlLoaderError::DataCorrupt;
  }

  return components::FontResource{std::move(maybeFont.result())};
}

}  // namespace donner::svg
