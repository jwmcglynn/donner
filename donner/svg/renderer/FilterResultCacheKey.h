#pragma once
/// @file
/// Identity of the content a cached filter result was rendered from.

#include <cstdint>
#include <memory>
#include <optional>

#include "donner/base/Box.h"
#include "donner/base/Vector2.h"

namespace donner::svg {

/**
 * What a filter layer's input and graph were derived from, independent of where the result lands
 * on the target.
 *
 * A renderer that caches filter results reuses one only for a key equal to the one it was
 * recorded under, and only for the same non-translation part of the device transform (see \ref
 * RendererInterface::pushCachedFilterLayer). Two draws with equal keys draw the same content
 * through the same filter graph, so their results differ only by where they are placed.
 *
 * The driver builds a key only for content whose rendering is a function of these fields: a
 * filtered subtree of a document drawn through an external `<use>`, whose styles are retained for
 * the occurrence, and that holds nothing loaded or animated independently of the document's
 * mutation revision.
 */
struct FilterResultContentKey {
  /// Document whose subtree is filtered. A key whose document has been destroyed matches nothing.
  std::weak_ptr<const void> document;
  /// The document's mutation revision; every DOM mutation changes it.
  std::uint64_t documentRevision = 0;
  /// The document's font resource revision; a font load changes it.
  std::uint64_t fontResourceRevision = 0;
  /// Epoch of the occurrence's retained styles. A restyle of the subtree changes it.
  std::uint64_t styleEpoch = 0;
  /// Root of the instance the occurrence draws.
  std::uint32_t instanceRoot = 0;
  /// Element whose filter produces the result.
  std::uint32_t filteredElement = 0;
  /// Viewport the `<use>` establishes for a referenced `<symbol>` or `<svg>`, if any.
  std::optional<Box2d> instanceViewport;
  /// The filter region, in the filtered element's user space.
  std::optional<Box2d> filterRegion;
  /// The viewport primitive lengths resolve against, in user space.
  std::optional<Box2d> primitiveViewport;
  /// User-space to pixel-space scale the filter graph was configured with.
  Vector2d userToPixelScale = Vector2d(1.0, 1.0);

  /// Whether \p other names the same content. False when either document is gone.
  [[nodiscard]] bool sameContent(const FilterResultContentKey& other) const {
    if (document.expired() || other.document.expired() || document.owner_before(other.document) ||
        other.document.owner_before(document)) {
      return false;
    }
    return documentRevision == other.documentRevision &&
           fontResourceRevision == other.fontResourceRevision && styleEpoch == other.styleEpoch &&
           instanceRoot == other.instanceRoot && filteredElement == other.filteredElement &&
           instanceViewport == other.instanceViewport && filterRegion == other.filterRegion &&
           primitiveViewport == other.primitiveViewport &&
           userToPixelScale == other.userToPixelScale;
  }
};

/// Outcome of \ref RendererInterface::pushCachedFilterLayer.
enum class CachedFilterLayerResult : std::uint8_t {
  /// A cached result was drawn. The caller draws none of the layer's content and does not pop it.
  Drawn,
  /// A filter layer was pushed as by \ref RendererInterface::pushFilterLayer. The caller draws the
  /// content and pops it.
  Pushed,
};

}  // namespace donner::svg
