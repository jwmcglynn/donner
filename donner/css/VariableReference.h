#pragma once
/// @file

#include <optional>
#include <span>
#include <string_view>

#include "donner/base/Utils.h"
#include "donner/css/ComponentValue.h"

namespace donner::css::details {

/// Parsed name and optional fallback borrowed from a var() function's arguments.
struct VariableReference {
  std::string_view name;
  std::optional<std::span<const ComponentValue>> fallback;
};

/// Parse the arguments of a known var() function.
std::optional<VariableReference> ParseVariableReference(
    const Function& function UTILS_LIFETIME_BOUND);

}  // namespace donner::css::details
