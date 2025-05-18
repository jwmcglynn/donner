#include "donner/svg/resources/FontLoader.h"

namespace donner::svg {

std::variant<std::vector<uint8_t>, UrlLoaderError> FontLoader::fromUri(std::string_view uri) {
  auto result = urlLoader_.fromUri(uri);
  if (std::holds_alternative<UrlLoaderError>(result)) {
    return std::get<UrlLoaderError>(result);
  }
  return std::get<UrlLoader::Result>(result).data;
}

}  // namespace donner::svg
