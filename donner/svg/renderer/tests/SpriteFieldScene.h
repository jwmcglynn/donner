#pragma once
/// @file
/// A game-like scene: many external `<use>` sprites, drawn from several sprite sheets, that move
/// every frame, with `setHref` retargets within a sheet and occasional `opacity` writes.

#include <optional>
#include <string>
#include <vector>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGUseElement.h"

namespace donner::svg::tests {

/// Shape of a sprite field.
struct SpriteFieldOptions {
  int sprites = 150;        //!< External `<use>` elements in the host document.
  int sheets = 7;           //!< Distinct external sprite-sheet documents.
  int spritesPerSheet = 8;  //!< Sprites in each sheet.
};

/// A parsed sprite field and the uses its frames drive.
struct SpriteFieldScene {
  SVGDocument document;             //!< Host document.
  std::vector<SVGUseElement> uses;  //!< Every external use, in document order.
  SpriteFieldOptions options;       //!< The options the scene was built with.
};

/// What a frame changes.
enum class SpriteFieldChange {
  Move,      //!< Every use gets a new transform.
  Opacity,   //!< A few uses get a new `opacity` attribute.
  Retarget,  //!< A few uses are pointed at another sprite of the same sheet.
  All,       //!< Moves and retargets every frame and fades every eighth, as a game does.
};

/**
 * Parses a sprite field whose sheets are served from memory. Returns `std::nullopt` and records a
 * test failure if the host does not parse.
 *
 * @param options Shape of the field.
 */
std::optional<SpriteFieldScene> MakeSpriteFieldScene(const SpriteFieldOptions& options = {});

/**
 * Applies frame \p frame of \p change through the DOM.
 *
 * @param scene Scene to animate.
 * @param frame Zero-based frame index.
 * @param change What the frame changes.
 */
void AdvanceSpriteField(SpriteFieldScene& scene, int frame,
                        SpriteFieldChange change = SpriteFieldChange::All);

}  // namespace donner::svg::tests
