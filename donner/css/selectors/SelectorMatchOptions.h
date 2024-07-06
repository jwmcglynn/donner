#pragma once
/// @file

#include "donner/css/SelectorTraversal.h"

namespace donner::css {

/**
 * Options for matching a selector against an element.
 *
 * This is used to pass additional information to the matching algorithm, such as the element to
 * match against for relative queries.
 *
 * @tparam T A type that fulfills the ElementLike concept, matching the \ref Selector::matches
 * method.
 */
template <traversal::ElementLike T>
struct SelectorMatchOptions {
  const T* relativeToElement =
      nullptr;  ///< Enables relative querying and uses this element as the reference point. For
                ///< example, `> div` will match `div` that is a child of `relativeToElement`.
  const T* scopeElement = nullptr;  ///< Element to match against `:scope` queries. Cannot be
                                    ///< matched directly, but can be used for relative matching.
};

}  // namespace donner::css
