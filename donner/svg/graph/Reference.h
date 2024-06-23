#pragma once
/// @file

#include <ostream>

#include "donner/base/RcString.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg {

/**
 * Represents a resolved reference to an SVG entity.
 *
 * Returned by \ref Reference::resolve.
 */
struct ResolvedReference {
  EntityHandle handle;  //!< Handle to the resolved entity

  /**
   * Implicit conversion operator to Entity
   *
   * @return The entity associated with the handle
   */
  operator Entity() const { return handle.entity(); }

  /// Returns true if this ResolvedReference is non-empty.
  bool valid() const { return bool(handle); }
};

/**
 * Represents a reference to an SVG entity by its href, typically created from a `url(#id)` string.
 *
 * The reference can be resolved to an entity using \ref resolve.
 *
 * | **Source** | **`href` value** |
 * |------------|------------------|
 * | `url(#id)` | `#id` |
 * | `href="#id` | `#id` |
 * | `xlink:href="#id"` | `#id` |
 * | `xlink:href="url(#id)"` | `url(#id)` (invalid syntax) |
 *
 * Note that absolute references, such as `path/to/other-file.svg#elementId`, are not supported.
 */
struct Reference {
  RcString href;  //!< The href string identifying the referenced entity, e.g. `#id`

  /**
   * Constructs a Reference from an RcString
   * @param href The href string
   */
  /* implicit */ Reference(const RcString& href) : href(href) {}

  /**
   * Constructs a Reference from a C-style string
   * @param href The href string
   */
  /* implicit */ Reference(const char* href) : href(href) {}

  /**
   * Attempts to resolve the reference using the provided registry.

   * @param registry The Registry to use for resolution
   * @return An optional ResolvedReference, which is empty if resolution fails
   */
  std::optional<ResolvedReference> resolve(Registry& registry) const;

  /// Equality operator.
  bool operator==(const Reference& other) const = default;

  /// Outputs the href string to a stream.
  friend std::ostream& operator<<(std::ostream& os, const Reference& ref) { return os << ref.href; }
};

}  // namespace donner::svg
