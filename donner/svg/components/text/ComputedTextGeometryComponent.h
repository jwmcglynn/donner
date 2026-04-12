#pragma once
/// @file

#include <entt/entity/entity.hpp>  // entt::entity, entt::null
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Vector2.h"
#include "donner/base/Path.h"
#include "donner/svg/text/TextTypes.h"

namespace donner::svg::components {

/**
 * Cached geometry for text layout and text-derived public API queries.
 *
 * Attached to the root `<text>` entity and populated from the shared text engine. This stores
 * per-glyph outlines and per-character metrics so DOM APIs can answer geometry queries without
 * duplicating text layout logic.
 */
struct ComputedTextGeometryComponent {
  /**
   * Outline geometry for a single rendered glyph.
   */
  struct GlyphGeometry {
    entt::entity sourceEntity = entt::null;  ///< Span source entity that owns this glyph.
    Path path;                         ///< Glyph outline in text-element local coordinates.
    Box2d extent;                             ///< Ink bounds in text-element local coordinates.
  };

  /**
   * Cached geometry for one addressable character.
   */
  struct CharacterGeometry {
    entt::entity sourceEntity = entt::null;     ///< Source entity that owns this character.
    Vector2d startPosition = Vector2d::Zero();  ///< Character start position in local coords.
    Vector2d endPosition = Vector2d::Zero();    ///< Character end position in local coords.
    Box2d extent = Box2d();                       ///< Character ink bounds in local coords.
    double rotation = 0.0;                      ///< Rotation in degrees.
    double advance = 0.0;                       ///< Advance magnitude.
    bool rendered = false;                      ///< True if any glyph geometry was produced.
    bool hasExtent = false;                     ///< True if extent contains real glyph bounds.
  };

  std::vector<GlyphGeometry> glyphs;          ///< Cached glyph outlines for the text root.
  std::vector<CharacterGeometry> characters;  ///< Cached character metrics in logical order.
  std::vector<TextRun> runs;                  ///< Cached layout runs for renderer reuse.
  Box2d inkBounds;                             ///< Union of glyph ink bounds.
  Box2d emBoxBounds;                           ///< Union of em-box bounds used for text bbox.
};

}  // namespace donner::svg::components
