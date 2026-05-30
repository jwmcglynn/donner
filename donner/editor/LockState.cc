#include "donner/editor/LockState.h"

#include <optional>
#include <string_view>

#include "donner/base/RcString.h"
#include "donner/base/xml/XMLQualifiedName.h"

namespace donner::editor {

bool IsLocked(const svg::SVGElement& element) {
  const xml::XMLQualifiedNameRef lockedName(kLockedAttributeName);
  for (std::optional<svg::SVGElement> node = element; node.has_value();
       node = node->parentElement()) {
    if (const std::optional<RcString> value = node->getAttribute(lockedName);
        value.has_value() && std::string_view(*value) == kLockedAttributeValue) {
      return true;
    }
  }
  return false;
}

}  // namespace donner::editor
