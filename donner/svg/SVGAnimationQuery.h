#pragma once
/// @file

namespace donner::svg {

class SVGElement;

/// Conservatively determine whether an animation may target \p element.
///
/// An animation element (\ref xml_animate, \ref xml_animateTransform, \ref xml_set) animates the
/// element named by its `href`, or its own parent when `href` is absent. A caller that must not
/// change how a document paints uses this to tell whether an element is observed by an animation:
/// a group holding an animation element is animated without naming itself anywhere, and moving a
/// child out of it changes what the animation applies to.
///
/// @param element Element to test.
/// @return True for a target or exhausted analysis; false only for a proven non-target.
/// Work is bounded. Exhaustion preserves structure by preventing callers from treating an
/// incomplete query as permission to move an element out of an animated container.
bool IsAnimationTarget(const SVGElement& element);

}  // namespace donner::svg
