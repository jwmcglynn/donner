#include "donner/svg/components/style/SelectorWalk.h"

#include <algorithm>
#include <cstddef>

#include "donner/base/StringUtils.h"

namespace donner::svg::components {

int CompareLowercased(std::string_view stored, std::string_view probe) {
  const std::size_t length = std::min(stored.size(), probe.size());
  for (std::size_t i = 0; i < length; ++i) {
    const char lhs = stored[i];
    const char rhs = StringUtils::ToLowerAscii(probe[i]);
    if (lhs != rhs) {
      return static_cast<unsigned char>(lhs) < static_cast<unsigned char>(rhs) ? -1 : 1;
    }
  }
  return stored.size() == probe.size() ? 0 : (stored.size() < probe.size() ? -1 : 1);
}

int CompareExact(std::string_view stored, std::string_view probe) {
  const int result = stored.compare(probe);
  return result < 0 ? -1 : (result > 0 ? 1 : 0);
}

RcString Lowercased(const RcString& name) {
  const bool hasUppercase =
      std::any_of(name.begin(), name.end(), [](char ch) { return ch >= 'A' && ch <= 'Z'; });
  return hasUppercase ? RcString(StringUtils::ToLowerAscii(std::string_view(name))) : name;
}

}  // namespace donner::svg::components
