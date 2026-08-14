#pragma once
/// @file
/// In-memory generator for the Donner editor showcase.

#include <string>
#include <string_view>

#include "donner/editor/ViewportSvgExport.h"

namespace donner::editor {

/**
 * Generate the v0.8 editor showcase from an SVG document.
 *
 * The generated SVG exercises the same text-to-outlines and viewport-export
 * paths as the interactive editor. It is returned in memory so demos and tests
 * can create the derived showcase on demand without checking generated assets
 * into the repository.
 *
 * @param source Base SVG source. The repository's canonical input is
 *   `donner_splash.svg`; callers must not supply live `<text>` or the
 *   generator's reserved `showcase_svg_label*` and `donner-editor-overlay`
 *   ids.
 * @return Generated showcase SVG, or a human-readable error.
 */
Result<std::string, std::string> GenerateShowcaseAsset(std::string_view source);

}  // namespace donner::editor
