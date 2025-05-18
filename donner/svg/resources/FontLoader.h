#pragma once
/// @file

#include <variant>
#include <vector>

#include "donner/svg/resources/UrlLoader.h"

namespace donner::svg {

class FontLoader {
public:
  explicit FontLoader(ResourceLoaderInterface& resourceLoader) : urlLoader_(resourceLoader) {}
  ~FontLoader() = default;

  FontLoader(const FontLoader&) = delete;
  FontLoader(FontLoader&&) = delete;
  FontLoader& operator=(const FontLoader&) = delete;
  FontLoader& operator=(FontLoader&&) = delete;

  std::variant<std::vector<uint8_t>, UrlLoaderError> fromUri(std::string_view uri);

private:
  UrlLoader urlLoader_;
};

}  // namespace donner::svg
