#pragma once
/// @file

namespace donner::svg::components {

/**
 * Controls how the attached element is rendered, determines how different element types are
 * rendered.
 */
enum class RenderingBehavior {
  Default,             ///< The default rendering behavior, which renders and traverses children.
  Nonrenderable,       ///< The element and its children are not rendered.
  NoTraverseChildren,  ///< The element is rendered, but its children are not traversed.
  ShadowOnlyChildren  ///< Only traverse this element's children within a shadow tree instantiation.
};

/**
 * Component that controls how the attached element is rendered, determines how different element
 * types are rendered.
 *
 * Allows changing behavior about if an element or children are rendered, and whether or not they
 * contribute to the transform tree.
 */
struct RenderingBehaviorComponent {
  /// The rendering behavior of the element, defaults to \ref RenderingBehavior::Default (renderable
  /// and traversable)
  RenderingBehavior behavior = RenderingBehavior::Default;
  /// Whether or not this element inherits its parent's transform.
  bool inheritsParentTransform = true;

  /// Constructor.
  explicit RenderingBehaviorComponent(RenderingBehavior behavior) : behavior(behavior) {}
};

}  // namespace donner::svg::components
