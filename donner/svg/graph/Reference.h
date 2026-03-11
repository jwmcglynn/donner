#pragma once
/// @file

#include <ostream>
#include <string_view>

#include "donner/base/EcsRegistry.h"
#include "donner/base/RcString.h"

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
 * Supports both same-document and external references:
 *
 * | **Source** | **`href` value** | **Type** |
 * |------------|------------------|----------|
 * | `url(#id)` | `#id` | Same-document |
 * | `href="#id"` | `#id` | Same-document |
 * | `href="file.svg"` | `file.svg` | External (whole document) |
 * | `href="file.svg#id"` | `file.svg#id` | External (element by ID) |
 *
 * Same-document references are resolved via \ref resolve(). External references require
 * loading the external document first; use \ref isExternal(), \ref documentUrl(), and
 * \ref fragment() to inspect the reference components, then resolve the fragment against
 * the external document's registry.
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
   * Returns true if this reference points to an external document (i.e., has a document URL
   * component). A reference like `#id` is same-document; `file.svg` or `file.svg#id` is external.
   */
  bool isExternal() const;

  /**
   * Returns the document URL component of the reference, or an empty string if this is a
   * same-document reference.
   *
   * Examples:
   * - `#id` → `""`
   * - `file.svg` → `"file.svg"`
   * - `file.svg#elementId` → `"file.svg"`
   * - `path/to/file.svg#id` → `"path/to/file.svg"`
   */
  std::string_view documentUrl() const;

  /**
   * Returns the fragment component of the reference (without the `#` prefix), or an empty
   * string if there is no fragment.
   *
   * Examples:
   * - `#id` → `"id"`
   * - `file.svg` → `""`
   * - `file.svg#elementId` → `"elementId"`
   */
  std::string_view fragment() const;

  /**
   * Attempts to resolve the reference as a same-document reference using the provided registry.
   * Only handles fragment-only references (`#id`). For external references, use \ref isExternal()
   * and load the external document separately.
   *
   * @param registry The Registry to use for resolution
   * @return An optional ResolvedReference, which is empty if resolution fails
   */
  std::optional<ResolvedReference> resolve(Registry& registry) const;

  /**
   * Resolves a fragment identifier against the given registry. Unlike \ref resolve(), this
   * does not require the href to start with `#` — it uses the \ref fragment() component directly.
   * This is used for resolving external references after loading the external document.
   *
   * @param registry The Registry to resolve the fragment against.
   * @return An optional ResolvedReference, which is empty if the fragment is empty or not found.
   */
  std::optional<ResolvedReference> resolveFragment(Registry& registry) const;

  /// Equality operator.
  bool operator==(const Reference& other) const = default;

  /// Outputs the href string to a stream.
  friend std::ostream& operator<<(std::ostream& os, const Reference& ref) { return os << ref.href; }
};

}  // namespace donner::svg
