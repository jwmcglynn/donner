#pragma once
/// @file

#include <cstddef>
#include <optional>

#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * Base class for elements which render or impact the rendering tree, such as \ref xml_path and \ref
 * xml_g.
 *
 * This cannot be instantiated directly.
 *
 * Holds a transformation which defaults to identity.
 */
class SVGGraphicsElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Inheriting constructor to be called by derived classes. \ref SVGGraphicsElement cannot be
  /// instantiated directly.
  explicit SVGGraphicsElement(EntityHandle handle);

public:
  /// Returns true if the given element type can be cast to \ref SVGGraphicsElement.
  static constexpr bool IsBaseOf(ElementType type) {
    return type == ElementType::A || type == ElementType::Circle || type == ElementType::Defs ||
           type == ElementType::Ellipse || type == ElementType::G || type == ElementType::Image ||
           type == ElementType::Line || type == ElementType::Path || type == ElementType::Polygon ||
           type == ElementType::Polyline || type == ElementType::Rect || type == ElementType::SVG ||
           type == ElementType::Switch || type == ElementType::Symbol ||
           type == ElementType::Text || type == ElementType::TextPath ||
           type == ElementType::TSpan || type == ElementType::Unknown;
  }

  /// Get the 2d transformation for this element, which maps element coordinates into the parent's
  /// coordinate space (parent-from-element). The `transform-origin` offset is not included.
  Transform2d transform() const;

  /// Set the 2d transformation for this element (parent-from-element), overriding any authored
  /// `transform`. The `transform-origin` offset still applies on top of it.
  void setTransform(const Transform2d& transform);

  /// Get the absolute transform that maps this element's coordinates into world (document)
  /// coordinates, including all ancestor transforms. Despite the name, this is the
  /// world-from-element transform; invert it to map world points into element space.
  Transform2d elementFromWorld() const;

  /// Return this element's single effective clip outline in document coordinates. The transform
  /// includes the referencing element's own transform, as required for userSpaceOnUse and
  /// objectBoundingBox clips; callers can distinguish an inherited clip by querying the ancestor
  /// that owns it. An absent, invalid, nested, or multi-shape boolean clip returns nullopt rather
  /// than a misleading source outline. If provided, `hasClipPath` distinguishes absence from an
  /// unsupported effective clip; callers combining ancestor clips must fail closed in the latter
  /// case. Explicit limits bound source and returned path storage. This reads the prepared render
  /// tree, and returns nullopt when this element has no visible rendering instance.
  ///
  /// @param maxVerbs Maximum number of path verbs accepted in the source clip path.
  /// @param maxPoints Maximum number of points accepted in the source clip path.
  /// @param maxBytes Maximum retained bytes for the source clip path and the returned outline.
  /// @param hasClipPath Optional output, set to true when the element has an effective clip path.
  std::optional<Path> resolvedSimpleClipPathOutline(std::size_t maxVerbs, std::size_t maxPoints,
                                                    std::size_t maxBytes,
                                                    bool* hasClipPath = nullptr) const;
};

}  // namespace donner::svg
