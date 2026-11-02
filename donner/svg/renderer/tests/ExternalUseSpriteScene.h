#pragma once
/// @file
/// An animated sprite drawn through an external `<use>`: the host document moves the `<use>` every
/// frame and periodically retargets it to another sprite in the same external sprite sheet.

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGUseElement.h"

namespace donner::svg::tests {

/// What each sprite in the external sprite sheet contains.
enum class SpriteKind {
  Bitmap,  //!< One embedded PNG `<image>` per sprite.
  Vector,  //!< Dozens of filled cubic `<path>` elements per sprite.
};

/// The sprite count of the sheet; every sprite lives in the same external document.
constexpr int kSpriteSheetSpriteCount = 12;

/// Frames between `setHref` calls that advance the animation to the next sprite.
constexpr int kFramesPerSpriteChange = 4;

/// URL the host documents use for their external resource.
constexpr std::string_view kExternalResourceUrl = "sprites.svg";

/// A host document whose only external resource is served from memory.
struct ExternalUseDocument {
  SVGDocument document;  //!< Host document.
  /// The external document once the host has loaded it, empty before.
  std::shared_ptr<SVGDocumentHandle> resource;
};

/// A parsed sprite scene plus the `<use>` the animation drives.
struct ExternalUseSpriteScene {
  ExternalUseDocument host;  //!< Host document and its loaded sprite sheet.
  SVGUseElement character;   //!< The animated external `<use>`.
};

/**
 * Parses \p hostSource with \ref kExternalResourceUrl served from \p resourceSource. Returns
 * `std::nullopt` and records a test failure if the host does not parse.
 *
 * @param hostSource Host document source.
 * @param resourceSource External document source.
 */
std::optional<ExternalUseDocument> MakeExternalUseDocument(std::string_view hostSource,
                                                           std::string resourceSource);

/// Builds the external sprite sheet source.
std::string MakeSpriteSheetSource(SpriteKind kind);

/// Parses the sprite host document. Returns `std::nullopt` and records a test failure if the host
/// does not parse.
std::optional<ExternalUseSpriteScene> MakeExternalUseSpriteScene(SpriteKind kind);

/// Moves the character for \p frame through `setTransform`, as an embedding application does
/// every frame.
void MoveExternalUseSprite(ExternalUseSpriteScene& scene, int frame);

/// Points the character at sprite \p sprite through `setHref`.
void SetExternalUseSprite(ExternalUseSpriteScene& scene, int sprite);

/**
 * Applies one animation frame through the DOM: moves the character every frame and calls `setHref`
 * every \ref kFramesPerSpriteChange frames.
 *
 * @param scene Scene to animate.
 * @param frame Zero-based frame index.
 */
void AdvanceExternalUseSpriteFrame(ExternalUseSpriteScene& scene, int frame);

}  // namespace donner::svg::tests
