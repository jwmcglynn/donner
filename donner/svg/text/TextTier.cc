#include "donner/svg/text/TextTier.h"

#include <ostream>

#include "donner/base/PackagedResource.h"

namespace donner::svg {

std::ostream& operator<<(std::ostream& os, TextTier tier) {
  switch (tier) {
    case TextTier::None: return os << "TextTier::None";
    case TextTier::Basic: return os << "TextTier::Basic";
    case TextTier::Full: return os << "TextTier::Full";
  }

  return os << "TextTier::<unknown>";
}

TextTier BuiltTextTier() {
#if defined(DONNER_TEXT_FULL)
  return TextTier::Full;
#elif defined(DONNER_TEXT_ENABLED)
  return TextTier::Basic;
#else
  return TextTier::None;
#endif
}

bool IsWoff2BuiltIn() {
#if defined(DONNER_TEXT_WOFF2_ENABLED)
  return true;
#else
  return false;
#endif
}

bool PrepareTextResources(std::string* error) {
  return !IsTextBuiltIn() || PreparePackagedResources(error);
}

}  // namespace donner::svg
