#pragma once
/// @file

#include <functional>
#include <optional>
#include <span>
#include <string_view>

#include "donner/css/ComponentValue.h"

namespace donner::editor {

/// A named reference as written in an authored CSS declaration.
struct CssSourceReference {
  enum class Kind { CustomProperty, FragmentUrl };
  Kind kind;
  std::string_view name;  ///< CSS-decoded custom name or fragment ID without '#'.
  SourceRange spelling;   ///< Bytes in the parsed CSS source, before mapping into XML.
};

/// Optional accounting and fallback boundaries when indexing authored references.
struct CssSourceReferenceHooks {
  std::function<bool()> consume;  ///< Charge one inspected component, or stop.
  std::function<void(std::string_view)> enterFallback;  ///< Enter fallback of this custom name.
  std::function<void()> leaveFallback;                  ///< Leave the fallback.
};

/// Visit CSS references in lexical order. The visitor returns false to stop traversal.
/// When available, the prepared-value predicate selects the active var() fallback. A missing
/// answer leaves the fallback unvisited. Without a predicate all authored fallbacks are visited.
bool VisitCssSourceReferences(
    std::span<const css::ComponentValue> values, std::string_view cssSource,
    const std::function<bool(const CssSourceReference&)>& visit,
    const std::function<std::optional<bool>(std::string_view)>& hasValue = {},
    const CssSourceReferenceHooks& hooks = {});

}  // namespace donner::editor
