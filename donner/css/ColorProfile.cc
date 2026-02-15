#include "donner/css/ColorProfile.h"

#include <algorithm>

namespace donner::css {

void ColorProfileRegistry::registerProfile(std::string profileName, ColorSpaceId id) {
  std::transform(profileName.begin(), profileName.end(), profileName.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  profiles_[profileName] = id;
}

std::optional<ColorSpaceId> ColorProfileRegistry::resolve(std::string_view profileName) const {
  std::string key(profileName);
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  auto it = profiles_.find(key);
  if (it == profiles_.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace donner::css
