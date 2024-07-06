#pragma once
/// @file

#include <optional>

#include "donner/base/SmallVector.h"
#include "donner/base/Utils.h"
#include "donner/svg/xml/XMLQualifiedName.h"

namespace donner {

/**
 * Concept for types that can be matched against a selector, such as a \ref donner::svg::SVGElement.
 *
 * The type must support tree traversal operations, such as `parentElement()` and
 * `previousSibling()`, and type and class information to match against the respective selectors.
 */
template <typename T>
concept ElementLike =
    requires(const T t, const T otherT, const svg::XMLQualifiedNameRef attribName) {
      { t.operator==(otherT) } -> std::same_as<bool>;
      { t.parentElement() } -> std::same_as<std::optional<T>>;
      { t.firstChild() } -> std::same_as<std::optional<T>>;
      { t.lastChild() } -> std::same_as<std::optional<T>>;
      { t.previousSibling() } -> std::same_as<std::optional<T>>;
      { t.nextSibling() } -> std::same_as<std::optional<T>>;
      { t.xmlTypeName() } -> std::same_as<svg::XMLQualifiedNameRef>;
      { t.isKnownType() } -> std::same_as<bool>;
      { t.id() } -> std::same_as<RcString>;
      { t.className() } -> std::same_as<RcString>;
      { t.getAttribute(attribName) } -> std::same_as<std::optional<RcString>>;
      {
        t.findMatchingAttributes(attribName)
      } -> std::same_as<SmallVector<svg::XMLQualifiedNameRef, 1>>;
    };

}  // namespace donner
