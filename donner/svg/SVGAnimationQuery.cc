#include "donner/svg/SVGAnimationQuery.h"

#include <array>
#include <optional>
#include <string_view>

#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/SVGAnimateElement.h"
#include "donner/svg/SVGAnimateTransformElement.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGSetElement.h"
#include "donner/svg/components/animation/AnimationSystem.h"

namespace donner::svg {
namespace {

/// Whether @p element is an animation element, by tag. Matching on the tag rather than the element
/// type is deliberate: without experimental parsing an animation element is retained as an unknown
/// element with its tag intact, and it still animates in a renderer that supports animation.
bool IsAnimationElementTag(const SVGElement& element) {
  static constexpr std::array<std::string_view, 3> kAnimationTags = {
      SVGAnimateElement::Tag,
      SVGAnimateTransformElement::Tag,
      SVGSetElement::Tag,
  };

  const RcString tagName(element.tagName().name);
  for (std::string_view tag : kAnimationTags) {
    if (tagName == tag) {
      return true;
    }
  }

  return false;
}

/// Whether @p animationElement targets @p element, resolving `href` (with or without a leading
/// `#`) and falling back to the parent element, the way the animation system does.
bool AnimationElementTargets(const SVGElement& animationElement, const SVGElement& element) {
  std::optional<RcString> href = animationElement.getAttribute("href");
  if (!href.has_value()) {
    href = animationElement.getAttribute(xml::XMLQualifiedNameRef("xlink", "href"));
  }

  if (href.has_value()) {
    std::string_view id = href->str();
    if (!id.empty() && id.front() == '#') {
      id.remove_prefix(1);
    }

    const RcString elementId = element.id();
    return !elementId.empty() && elementId == id;
  }

  const std::optional<SVGElement> parent = animationElement.parentElement();
  return parent.has_value() && *parent == element;
}

/// The topmost element of @p element's tree.
SVGElement RootOf(const SVGElement& element) {
  SVGElement current = element;
  for (std::optional<SVGElement> parent = current.parentElement(); parent.has_value();
       parent = current.parentElement()) {
    current = *parent;
  }

  return current;
}

/// Whether any animation element in the tree rooted at @p current targets @p element.
bool AnyAnimationElementTargets(const SVGElement& current, const SVGElement& element) {
  if (IsAnimationElementTag(current) && AnimationElementTargets(current, element)) {
    return true;
  }

  for (std::optional<SVGElement> child = current.firstChild(); child.has_value();
       child = child->nextSibling()) {
    if (AnyAnimationElementTargets(*child, element)) {
      return true;
    }
  }

  return false;
}

}  // namespace

bool IsAnimationTarget(const SVGElement& element) {
  const bool resolvedTarget = element.withReadAccess([](DocumentReadAccess&, EntityHandle handle) {
    if (!handle) {
      return false;
    }

    return components::AnimationSystem().isAnimationTarget(*handle.registry(), handle.entity());
  });
  if (resolvedTarget) {
    return true;
  }

  // An animation element only carries animation components when the document was parsed with
  // experimental support, so the resolved state above can be empty for a document that still
  // animates elsewhere. Fall back to the element tags, which survive either parse.
  return AnyAnimationElementTargets(RootOf(element), element);
}

}  // namespace donner::svg
