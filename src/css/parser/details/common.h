#pragma once

#include <cctype>
#include <concepts>
#include <string_view>

namespace donner {
namespace css {
namespace details {

static bool stringLowercaseEq(std::string_view str, std::string_view matcher) {
  if (str.size() != matcher.size()) {
    return false;
  }

  for (size_t i = 0; i < str.size(); ++i) {
    if (std::tolower(str[i]) != matcher[i]) {
      return false;
    }
  }

  return true;
}

}  // namespace details
}  // namespace css
}  // namespace donner
