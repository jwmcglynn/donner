#pragma once
/// @file
/// A sprite-driven test scene: a host document that draws many sprites through external `<use>`
/// references into a few sprite-sheet documents, the way a game scene built from sprite sheets
/// does.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"

namespace donner::svg::tests {

/// Shape of a \ref SpriteScene.
struct SpriteSceneOptions {
  int width = 1206;               //!< Host width in pixels.
  int height = 2622;              //!< Host height in pixels.
  int sprites = 150;              //!< External `<use>` occurrences in the host.
  int glowingEvery = 4;           //!< Every Nth occurrence references a glowing sprite; 0 for none.
  bool includeText = false;       //!< Whether one sheet has a sprite with a text label.
  bool unreferencedText = false;  //!< Whether each sheet carries a text element nothing uses.
};

/**
 * Host document plus in-memory sprite sheets. The sheets are served by a resource loader owned by
 * the host document, so they load the way files next to the host would.
 */
class SpriteScene {
public:
  /// Builds and parses the scene. @param options Scene shape.
  explicit SpriteScene(const SpriteSceneOptions& options = {});

  /// The host document.
  SVGDocument& document() { return document_; }

  /// Number of external `<use>` occurrences.
  int spriteCount() const { return static_cast<int>(uses_.size()); }

  /// Moves every occurrence to its position for \p frame.
  void advance(int frame);

  /// The occurrence at \p index.
  SVGElement& use(int index) { return uses_[static_cast<size_t>(index)]; }

  /// Host source, for tests that render it through another path.
  const std::string& hostSource() const { return hostSource_; }

  /// Sheet sources keyed by URL.
  const std::map<std::string, std::string, std::less<>>& sheets() const { return sheets_; }

  /// Transform of occurrence \p index at \p frame.
  std::string transformFor(int index, int frame) const;

private:
  SpriteSceneOptions options_;
  std::map<std::string, std::string, std::less<>> sheets_;
  std::string hostSource_;
  SVGDocument document_;
  std::vector<SVGElement> uses_;
};

}  // namespace donner::svg::tests
