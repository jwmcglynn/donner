#pragma once
/// @file
/// Identity of the content a cached filter result was rendered from.

#include <cstdint>
#include <memory>
#include <optional>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"

namespace donner::svg {

namespace components {
struct FilterGraph;
}  // namespace components

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
  /// Painted bounds of the filtered element in its user space, strokes included, which tell a
  /// backend how far the graph's reach stays inside the filter region. Unset when they are not
  /// known, such as under a filtered descendant. Determined by the content, so not compared.
  std::optional<Box2d> contentBounds;
  /// Whether the key names a whole sprite occurrence (see \ref RendererInterface::drawCachedSprite)
  /// rather than one filter layer. For a sprite, \ref styleEpoch identifies the occurrence's
  /// inputs and \ref filterRegion holds the sprite's bounds in the `<use>` element's user space.
  bool sprite = false;

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
           userToPixelScale == other.userToPixelScale && sprite == other.sprite;
  }
};

/**
 * A sprite whose painting is one filtered element, whose filter result may be resampled at any
 * sub-pixel offset under the rules for cached filter results (see
 * \ref RendererInterface::pushCachedFilterLayer): then the whole sprite may be too, instead of
 * being kept per quarter-pixel bucket.
 */
struct SpriteResampling {
  /// The filtered element's single filter stage. Must outlive the call it is passed to.
  const components::FilterGraph* graph = nullptr;
  /// Maps the filtered element's user space into the sprite's (the `<use>` element's) user space.
  Transform2d spriteFromFilter;
  /// Painted bounds of the filtered element in its user space, strokes included.
  std::optional<Box2d> contentBounds;
};

/// Outcome of \ref RendererInterface::drawCachedSprite.
enum class CachedSpriteResult : std::uint8_t {
  /// The cached sprite was drawn. The caller draws none of the occurrence's content.
  Drawn,
  /// Nothing is cached for the sprite, but the backend admits it. The caller draws the content
  /// between \ref RendererInterface::pushSpriteRecording and
  /// \ref RendererInterface::popSpriteRecording.
  Record,
  /// Nothing is cached and the sprite is not recorded. The caller draws the content as usual.
  Uncached,
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
