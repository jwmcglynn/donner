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
  /// Syntactic form of the reference.
  enum class Kind {
    CustomProperty,  ///< A custom-property identifier inside var().
    FragmentUrl,     ///< A document-local URL fragment.
  };
  Kind kind;              ///< Syntactic form of this reference.
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
/// @param values Parsed CSS component values to inspect.
/// @param cssSource Authored CSS against which the components' offsets were measured.
/// @param visit Callback for each reference. Borrowed names are valid only during traversal.
/// @param hasValue Optional predicate for a usable prepared custom-property value.
/// @param hooks Optional callbacks to charge work and track entry to and exit from fallbacks.
/// @return True if traversal completed, or false if the visitor or accounting stopped it.
bool VisitCssSourceReferences(
    std::span<const css::ComponentValue> values, std::string_view cssSource,
    const std::function<bool(const CssSourceReference&)>& visit,
    const std::function<std::optional<bool>(std::string_view)>& hasValue = {},
    const CssSourceReferenceHooks& hooks = {});

}  // namespace donner::editor
